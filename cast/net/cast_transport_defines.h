// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_CAST_NET_CAST_TRANSPORT_DEFINES_H_
#define MEDIA_CAST_NET_CAST_TRANSPORT_DEFINES_H_

#include <stdint.h>

#include <map>
#include <set>
#include <string>

#include "base/basictypes.h"
#include "base/time/time.h"

namespace media {
namespace cast {

// TODO(mikhal): Implement and add more types.
enum CastTransportStatus {
  TRANSPORT_AUDIO_UNINITIALIZED = 0,
  TRANSPORT_VIDEO_UNINITIALIZED,
  TRANSPORT_AUDIO_INITIALIZED,
  TRANSPORT_VIDEO_INITIALIZED,
  TRANSPORT_INVALID_CRYPTO_CONFIG,
  TRANSPORT_SOCKET_ERROR,
  CAST_TRANSPORT_STATUS_LAST = TRANSPORT_SOCKET_ERROR
};

const size_t kMaxIpPacketSize = 1500;
// Each uint16 represents one packet id within a cast frame.
typedef std::set<uint16> PacketIdSet;
// Each uint8 represents one cast frame.
typedef std::map<uint8, PacketIdSet> MissingFramesAndPacketsMap;

// Rtcp defines.

enum RtcpPacketFields {
  kPacketTypeLow = 194,  // SMPTE time-code mapping.
  kPacketTypeSenderReport = 200,
  kPacketTypeReceiverReport = 201,
  kPacketTypeApplicationDefined = 204,
  kPacketTypeGenericRtpFeedback = 205,
  kPacketTypePayloadSpecific = 206,
  kPacketTypeXr = 207,
  kPacketTypeHigh = 210,  // Port Mapping.
};

enum RtcpPacketField {
    kRtcpSr     = 0x0002,
    kRtcpRr     = 0x0004,
    kRtcpDlrr   = 0x0400,
    kRtcpRrtr   = 0x0800,
    kRtcpCast   = 0x20000,
    kRtcpReceiverLog = 0x80000,
  };

// Each uint16 represents one packet id within a cast frame.
typedef std::set<uint16> PacketIdSet;
// Each uint8 represents one cast frame.
typedef std::map<uint8, PacketIdSet> MissingFramesAndPacketsMap;

// TODO(miu): UGLY IN-LINE DEFINITION IN HEADER FILE!  Move to appropriate
// location, separated into .h and .cc files.
class FrameIdWrapHelper {
 public:
  FrameIdWrapHelper()
      : first_(true), frame_id_wrap_count_(0), range_(kLowRange) {}

  uint32 MapTo32bitsFrameId(const uint8 over_the_wire_frame_id) {
    if (first_) {
      first_ = false;
      if (over_the_wire_frame_id == 0xff) {
        // Special case for startup.
        return kStartFrameId;
      }
    }

    uint32 wrap_count = frame_id_wrap_count_;
    switch (range_) {
      case kLowRange:
        if (over_the_wire_frame_id > kLowRangeThreshold &&
            over_the_wire_frame_id < kHighRangeThreshold) {
          range_ = kMiddleRange;
        }
        if (over_the_wire_frame_id >= kHighRangeThreshold) {
          // Wrap count was incremented in High->Low transition, but this frame
          // is 'old', actually from before the wrap count got incremented.
          --wrap_count;
        }
        break;
      case kMiddleRange:
        if (over_the_wire_frame_id >= kHighRangeThreshold) {
          range_ = kHighRange;
        }
        break;
      case kHighRange:
        if (over_the_wire_frame_id <= kLowRangeThreshold) {
          // Wrap-around detected.
          range_ = kLowRange;
          ++frame_id_wrap_count_;
          // Frame triggering wrap-around so wrap count should be incremented as
          // as well to match |frame_id_wrap_count_|.
          ++wrap_count;
        }
        break;
    }
    return (wrap_count << 8) + over_the_wire_frame_id;
  }

 private:
  enum Range { kLowRange, kMiddleRange, kHighRange, };

  static const uint8 kLowRangeThreshold = 63;
  static const uint8 kHighRangeThreshold = 192;
  static const uint32 kStartFrameId = UINT32_C(0xffffffff);

  bool first_;
  uint32 frame_id_wrap_count_;
  Range range_;

  DISALLOW_COPY_AND_ASSIGN(FrameIdWrapHelper);
};

inline uint32 GetVideoRtpTimestamp(const base::TimeTicks& time_ticks) {
  base::TimeTicks zero_time;
  base::TimeDelta recorded_delta = time_ticks - zero_time;
  // Timestamp is in 90 KHz for video.
  return static_cast<uint32>(recorded_delta.InMilliseconds() * 90);
}

}  // namespace cast
}  // namespace media

#endif  // MEDIA_CAST_NET_CAST_TRANSPORT_DEFINES_H_
