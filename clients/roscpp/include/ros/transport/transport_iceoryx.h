/**
 * @file transport_iceoryx.h
 *
 * Copyright 2020
 * Carnegie Robotics, LLC
 * 4501 Hatfield Street, Pittsburgh, PA 15201
 * http://www.carnegierobotics.com
 *
 * Significant history (date, user, job code, action):
 *   2020-09-26, zparker@carnegierobotics.com, 2040, Created file.
 */

#ifndef ROSCPP_TRANSPORT_ICEORYX_H
#define ROSCPP_TRANSPORT_ICEORYX_H

#include <ros/types.h>
#include <ros/transport/transport.h>

#include "ros/io.h"
#include "ros/poll_manager.h"
#include <ros/common.h>

#include <memory>

namespace ros
{

class TransportIceoryx;
typedef boost::shared_ptr<TransportIceoryx> TransportIceoryxPtr;

class PollSet;

/**
 * \brief IceoryxROS transport
 */
class ROSCPP_DECL TransportIceoryx : public Transport
{
public:
  enum TransportType
  {
    /**
     * Publishes to a topic.
     */
    Publisher,
    /**
     * Subscribes to a topic.
     */
    Subscriber,
    /**
     * Handles notifications for routing purposes.
     */
    Server
  };

  explicit TransportIceoryx(const PollManagerPtr& poll_mgr, std::string runtime = { });
  TransportIceoryx(const PollManagerPtr& poll_mgr, TransportType type, std::string topic, std::string runtime = { }, int id = 0);

  ~TransportIceoryx();

  // overrides from Transport
  virtual int32_t read(uint8_t* buffer, uint32_t size);
  virtual int32_t write(uint8_t* buffer, uint32_t size);

  virtual void enableWrite() { write_handled_ = write_enabled_ = true; }
  virtual void disableWrite() { write_enabled_ = false; }
  virtual void enableRead() { read_enabled_ = true; }
  virtual void disableRead() { read_enabled_ = false; }

  virtual void close();

  virtual const char* getType() { return "IceoryxROS"; }

  virtual std::string getTransportInfo();

private:
  class Private;

  void initOryx();

  void poll();

  std::unique_ptr<Private> d_;
  PollManagerPtr poll_manager_;
  boost::signals2::scoped_connection poll_conn_;
  std::string segment_name_;
  std::string topic_;
  TransportType type_;
  bool read_enabled_;
  bool write_enabled_;
  bool write_handled_;
};

}

#endif
