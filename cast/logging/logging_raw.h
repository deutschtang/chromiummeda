// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_CAST_LOGGING_LOGGING_RAW_H_
#define MEDIA_CAST_LOGGING_LOGGING_RAW_H_

#include <map>
#include <string>
#include <vector>

#include "base/basictypes.h"
#include "base/memory/linked_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/non_thread_safe.h"
#include "base/time/tick_clock.h"
#include "media/cast/logging/logging_defines.h"

namespace media {
namespace cast {

// This class is not thread safe, and should only be called from the main
// thread.
class LoggingRaw : public base::NonThreadSafe,
                   public base::SupportsWeakPtr<LoggingRaw> {
 public:
  explicit LoggingRaw(bool is_sender);
  ~LoggingRaw();

  // Inform of new event: three types of events: frame, packets and generic.
  // Frame events can be inserted with different parameters.
  void InsertFrameEvent(const base::TimeTicks& time_of_event,
                        CastLoggingEvent event,
                        uint32 rtp_timestamp,
                        uint32 frame_id);

  // Size - Inserting the size implies that this is an encoded frame.
  void InsertFrameEventWithSize(const base::TimeTicks& time_of_event,
                                CastLoggingEvent event,
                                uint32 rtp_timestamp,
                                uint32 frame_id,
                                int frame_size);

  // Render/playout delay
  void InsertFrameEventWithDelay(const base::TimeTicks& time_of_event,
                                 CastLoggingEvent event,
                                 uint32 rtp_timestamp,
                                 uint32 frame_id,
                                 base::TimeDelta delay);

  // Insert a packet event.
  void InsertPacketEvent(const base::TimeTicks& time_of_event,
                         CastLoggingEvent event,
                         uint32 rtp_timestamp,
                         uint32 frame_id,
                         uint16 packet_id,
                         uint16 max_packet_id,
                         size_t size);

  void InsertGenericEvent(const base::TimeTicks& time_of_event,
                          CastLoggingEvent event,
                          int value);

  // Get raw log data.
  FrameRawMap GetFrameData() const;
  PacketRawMap GetPacketData() const;
  GenericRawMap GetGenericData() const;

  AudioRtcpRawMap GetAndResetAudioRtcpData();
  VideoRtcpRawMap GetAndResetVideoRtcpData();

  // Reset all log data; except the Rtcp copies.
  void Reset();

 private:
  void InsertBaseFrameEvent(const base::TimeTicks& time_of_event,
                            CastLoggingEvent event,
                            uint32 frame_id,
                            uint32 rtp_timestamp);

  void InsertRtcpFrameEvent(const base::TimeTicks& time_of_event,
                            CastLoggingEvent event,
                            uint32 rtp_timestamp,
                            base::TimeDelta delay);

  const bool is_sender_;
  FrameRawMap frame_map_;
  PacketRawMap packet_map_;
  GenericRawMap generic_map_;
  AudioRtcpRawMap audio_rtcp_map_;
  VideoRtcpRawMap video_rtcp_map_;

  base::WeakPtrFactory<LoggingRaw> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(LoggingRaw);
};

}  // namespace cast
}  // namespace media

#endif  // MEDIA_CAST_LOGGING_LOGGING_RAW_H_

