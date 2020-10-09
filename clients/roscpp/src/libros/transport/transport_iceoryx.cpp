/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2008, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 */

#include "ros/file_log.h"
#include "ros/header.h"
#include "ros/io.h"
#include "ros/poll_set.h"
#include "ros/transport/transport_iceoryx.h"
#include <ros/assert.h>

#include <boost/bind.hpp>
#include <iceoryx_posh/popo/publisher.hpp>
#include <iceoryx_posh/popo/subscriber.hpp>
#include <iceoryx_posh/runtime/posh_runtime.hpp>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <sstream>
#include <string>

namespace ros
{

namespace
{

constexpr const char ServiceName[] = "ros_transport";
constexpr const char InstanceName[] = "0";

enum { UseDynamicPayloadSize = true };

std::string defaultRuntimeName()
{
  std::stringstream stream;
  stream << "/" << getpid() << "-" << gettid();
  return stream.str();
}

iox::runtime::IdString iceString(const std::string& str)
{
  iox::runtime::IdString conv;

  if (!conv.unsafe_assign(str))
  {
    std::stringstream stream;
    stream << "IceoryxTransport::iceString: string \"" <<
      str << "\" (len " << str.size() <<
      ") is too long for an IdString (capacity " <<
      std::to_string(conv.capacity()) << ")";

    throw std::runtime_error(stream.str());
  }

  return conv;
}

}

class TransportIceoryx::Private
{
public:
  explicit Private(const std::string& runtime)
  : runtime_{nullptr}
  , pub_{nullptr}
  , sub_{nullptr}
  {
    runtime_ = &iox::runtime::PoshRuntime::getInstance(runtime);
  }

  ~Private() noexcept
  {
    if (sub_ && sub_->getSubscriptionState() != iox::popo::SubscriptionState::NOT_SUBSCRIBED)
      sub_->unsubscribe();

    if (pub_)
      pub_->stopOffer();
  }

  void subscribe(const std::string& topic)
  {
    auto s = std::make_unique<iox::popo::Subscriber>(iox::capro::ServiceDescription{
        iceString(ServiceName),
        iceString(InstanceName),
        iceString(topic)
      });
    s->subscribe();

    sub_ = std::move(s);
  }

  void offer(const std::string& topic)
  {
    auto p = std::make_unique<iox::popo::Publisher>(iox::capro::ServiceDescription{
        iceString(ServiceName),
        iceString(InstanceName),
        iceString(topic)
      });
    p->offer();

    pub_ = std::move(p);
  }

  bool readable() const noexcept
  {
    return sub_ && sub_->hasNewChunks();
  }

  bool writeable() const noexcept
  {
    return !!pub_;
  }

  int32_t read(void* buf, size_t len)
  {
    if (!sub_)
      throw std::runtime_error("TransportIceoryx::Private: read called on non-subscribed instance");

    const iox::mepoo::ChunkHeader* header{ nullptr };
    if (!sub_->getChunk(&header))
      return 0;

    auto copy_size = std::min(len, static_cast<size_t>(header->m_info.m_payloadSize));
    if (copy_size > len)
      ROSCPP_LOG_DEBUG("TransportIceoryx::Private: read buffer is not large-enough for payload - truncating");

    std::memcpy(buf, header->payload(), copy_size);

    sub_->releaseChunk(header);

    return static_cast<int32_t>(
      std::min(static_cast<size_t>(std::numeric_limits<int32_t>::max()), copy_size));
  }

  int32_t write(const void* buf, uint32_t len)
  {
    if (!pub_)
      throw std::runtime_error("TransportIceoryx::Private: write called on non-offered instance");

    auto chunk = pub_->allocateChunk(len, UseDynamicPayloadSize);
    if (!chunk)
    {
      ROSCPP_LOG_DEBUG("TransportIceoryx::Private: unable to allocate %u bytes for writing", len);
      return -1;
    }

    std::memcpy(chunk, buf, len);

    pub_->sendChunk(chunk);

    return static_cast<int32_t>(len);
  }

  void close()
  {
    ROSCPP_LOG_DEBUG("%s closing pub %p and sub %p\n", __PRETTY_FUNCTION__, pub_.get(), sub_.get());

    if (pub_)
      pub_->stopOffer();

    pub_.reset();

    if (sub_)
      sub_->unsubscribe();

    sub_.reset();
  }

private:
  iox::runtime::PoshRuntime* runtime_;
  std::unique_ptr<iox::popo::Publisher> pub_;
  std::unique_ptr<iox::popo::Subscriber> sub_;
};

TransportIceoryx::TransportIceoryx(
  const PollManagerPtr& poll_mgr,
  std::string runtime)
: TransportIceoryx(poll_mgr, TransportType::Server, { }, runtime)
{
}

TransportIceoryx::TransportIceoryx(
  const PollManagerPtr& poll_mgr,
  TransportIceoryx::TransportType type,
  std::string topic,
  std::string runtime,
  int)
: d_{nullptr}
, poll_manager_{poll_mgr}
, poll_conn_{}
, segment_name_{runtime}
, topic_{topic}
, type_{type}
, read_enabled_{false}
, write_enabled_{false}
, write_handled_{false}
{
  if (runtime.empty())
    segment_name_ = defaultRuntimeName();

  d_ = std::make_unique<Private>(segment_name_);

  initOryx();
}

// make sure we have an out-of-line dtor due to usage of unique_ptr pimpl.
TransportIceoryx::~TransportIceoryx() = default;

int32_t TransportIceoryx::read(uint8_t* buffer, uint32_t size)
{
  return d_->read(buffer, size);
}

int32_t TransportIceoryx::write(uint8_t* buffer, uint32_t size)
{
  write_handled_ = true;

  return d_->write(buffer, size);
}

void TransportIceoryx::close()
{
  d_->close();
}

std::string TransportIceoryx::getTransportInfo()
{
  std::stringstream str;
  str << "Iceoryx " << (type_ == TransportType::Publisher ? "publisher" : "subscriber") << " connection on segment " << segment_name_ << " for topic " << topic_ << "\n";

  return str.str();
}

void TransportIceoryx::initOryx()
{
  if (poll_manager_)
    poll_conn_ = poll_manager_->addPollThreadListener(boost::bind(&TransportIceoryx::poll, this));

  switch (type_)
  {
    case TransportType::Publisher:
      d_->offer(topic_);
      write_enabled_ = true;
      break;
    case TransportType::Subscriber:
      d_->subscribe(topic_);
      read_enabled_ = true;
      break;
    default:
      break;
  }
}

void TransportIceoryx::poll()
{
  if (read_enabled_ && read_cb_ && d_->readable())
    read_cb_(shared_from_this());

  if (write_enabled_ && write_cb_ && d_->writeable())
  {
    if (write_handled_)
    {
      write_cb_(shared_from_this());

      // don't notify as writeable again until a write has been issued.
      write_handled_ = false;
    }
    else
    {
      ROSCPP_LOG_DEBUG("%s iceoryx %s write pending", __func__, topic_.c_str());
    }
  }
}

}
