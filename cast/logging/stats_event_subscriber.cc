// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/cast/logging/stats_event_subscriber.h"

#include "base/format_macros.h"
#include "base/logging.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"

#define STAT_ENUM_TO_STRING(enum) \
  case enum:                      \
    return #enum

namespace media {
namespace cast {

namespace {

using media::cast::CastLoggingEvent;
using media::cast::EventMediaType;

const size_t kMaxPacketEventTimeMapSize = 1000;

bool IsReceiverEvent(CastLoggingEvent event) {
  return event == FRAME_DECODED
      || event == FRAME_PLAYOUT
      || event == FRAME_ACK_SENT
      || event == PACKET_RECEIVED;
}

}  // namespace

StatsEventSubscriber::SimpleHistogram::SimpleHistogram(int64 min,
                                                       int64 max,
                                                       int64 width)
    : min_(min), max_(max), width_(width), buckets_((max - min) / width + 2) {
  CHECK_GT(buckets_.size(), 2u);
  CHECK_EQ(0, (max_ - min_) % width_);
}

StatsEventSubscriber::SimpleHistogram::~SimpleHistogram() {
}

void StatsEventSubscriber::SimpleHistogram::Add(int64 sample) {
  if (sample < min_) {
    ++buckets_.front();
  } else if (sample >= max_) {
    ++buckets_.back();
  } else {
    size_t index = 1 + (sample - min_) / width_;
    DCHECK_LT(index, buckets_.size());
    ++buckets_[index];
  }
}

void StatsEventSubscriber::SimpleHistogram::Reset() {
  buckets_.assign(buckets_.size(), 0);
}

scoped_ptr<base::ListValue>
StatsEventSubscriber::SimpleHistogram::GetHistogram() const {
  scoped_ptr<base::ListValue> histo(new base::ListValue);

  scoped_ptr<base::DictionaryValue> bucket(new base::DictionaryValue);

  bucket->SetString("bucket", base::StringPrintf("< %" PRId64, min_));
  bucket->SetInteger("count", buckets_.front());
  histo->Append(bucket.release());

  for (size_t i = 1; i < buckets_.size() - 1; i++) {
    bucket.reset(new base::DictionaryValue);

    int64 lower = min_ + (i - 1) * width_;
    int64 upper = lower + width_ - 1;
    bucket->SetString(
        "bucket", base::StringPrintf("%" PRId64 " - %" PRId64, lower, upper));
    bucket->SetInteger("count", buckets_[i]);
    histo->Append(bucket.release());
  }

  bucket.reset(new base::DictionaryValue);

  bucket->SetString("bucket", base::StringPrintf(">= %" PRId64, max_));
  bucket->SetInteger("count", buckets_.back());
  histo->Append(bucket.release());

  return histo.Pass();
}

StatsEventSubscriber::StatsEventSubscriber(
    EventMediaType event_media_type,
    base::TickClock* clock,
    ReceiverTimeOffsetEstimator* offset_estimator)
    : event_media_type_(event_media_type),
      clock_(clock),
      offset_estimator_(offset_estimator),
      network_latency_datapoints_(0),
      e2e_latency_datapoints_(0),
      num_frames_dropped_by_encoder_(0),
      num_frames_late_(0),
      start_time_(clock_->NowTicks()) {
  DCHECK(event_media_type == AUDIO_EVENT || event_media_type == VIDEO_EVENT);

  InitHistograms();
}

StatsEventSubscriber::~StatsEventSubscriber() {
  DCHECK(thread_checker_.CalledOnValidThread());
}

void StatsEventSubscriber::OnReceiveFrameEvent(const FrameEvent& frame_event) {
  DCHECK(thread_checker_.CalledOnValidThread());

  CastLoggingEvent type = frame_event.type;
  if (frame_event.media_type != event_media_type_)
    return;

  FrameStatsMap::iterator it = frame_stats_.find(type);
  if (it == frame_stats_.end()) {
    FrameLogStats stats;
    stats.event_counter = 1;
    stats.sum_size = frame_event.size;
    stats.sum_delay = frame_event.delay_delta;
    frame_stats_.insert(std::make_pair(type, stats));
  } else {
    ++(it->second.event_counter);
    it->second.sum_size += frame_event.size;
    it->second.sum_delay += frame_event.delay_delta;
  }

  bool is_receiver_event = IsReceiverEvent(type);
  UpdateFirstLastEventTime(frame_event.timestamp, is_receiver_event);

  if (type == FRAME_CAPTURE_BEGIN) {
    RecordFrameCaptureTime(frame_event);
  } else if (type == FRAME_CAPTURE_END) {
    RecordCaptureLatency(frame_event);
  } else if (type == FRAME_ENCODED) {
    RecordEncodeLatency(frame_event);
  } else if (type == FRAME_ACK_SENT) {
    RecordFrameTxLatency(frame_event);
  } else if (type == FRAME_PLAYOUT) {
    RecordE2ELatency(frame_event);
    base::TimeDelta delay_delta = frame_event.delay_delta;
    histograms_[PLAYOUT_DELAY_MS_HISTO]->Add(delay_delta.InMillisecondsF());
    if (delay_delta <= base::TimeDelta())
      num_frames_late_++;
  }

  if (is_receiver_event)
    UpdateLastResponseTime(frame_event.timestamp);
}

void StatsEventSubscriber::OnReceivePacketEvent(
    const PacketEvent& packet_event) {
  DCHECK(thread_checker_.CalledOnValidThread());

  CastLoggingEvent type = packet_event.type;
  if (packet_event.media_type != event_media_type_)
    return;

  PacketStatsMap::iterator it = packet_stats_.find(type);
  if (it == packet_stats_.end()) {
    PacketLogStats stats;
    stats.event_counter = 1;
    stats.sum_size = packet_event.size;
    packet_stats_.insert(std::make_pair(type, stats));
  } else {
    ++(it->second.event_counter);
    it->second.sum_size += packet_event.size;
  }

  bool is_receiver_event = IsReceiverEvent(type);
  UpdateFirstLastEventTime(packet_event.timestamp, is_receiver_event);

  if (type == PACKET_SENT_TO_NETWORK ||
      type == PACKET_RECEIVED) {
    RecordNetworkLatency(packet_event);
  } else if (type == PACKET_RETRANSMITTED) {
    // We only measure network latency using packets that doesn't have to be
    // retransmitted as there is precisely one sent-receive timestamp pairs.
    ErasePacketSentTime(packet_event);
  }

  if (is_receiver_event)
    UpdateLastResponseTime(packet_event.timestamp);
}

void StatsEventSubscriber::UpdateFirstLastEventTime(base::TimeTicks timestamp,
                                                    bool is_receiver_event) {
  if (is_receiver_event) {
    base::TimeDelta receiver_offset;
    if (!GetReceiverOffset(&receiver_offset))
      return;
    timestamp -= receiver_offset;
  }

  if (first_event_time_.is_null()) {
    first_event_time_ = timestamp;
  } else {
    first_event_time_ = std::min(first_event_time_, timestamp);
  }
  if (last_event_time_.is_null()) {
    last_event_time_ = timestamp;
  } else {
    last_event_time_ = std::max(last_event_time_, timestamp);
  }
}

scoped_ptr<base::DictionaryValue> StatsEventSubscriber::GetStats() const {
  StatsMap stats_map;
  GetStatsInternal(&stats_map);
  scoped_ptr<base::DictionaryValue> ret(new base::DictionaryValue);

  scoped_ptr<base::DictionaryValue> stats(new base::DictionaryValue);
  for (StatsMap::const_iterator it = stats_map.begin(); it != stats_map.end();
       ++it) {
    stats->SetDouble(CastStatToString(it->first), it->second);
  }
  for (HistogramMap::const_iterator it = histograms_.begin();
       it != histograms_.end();
       ++it) {
    stats->Set(CastStatToString(it->first),
               it->second->GetHistogram().release());
  }

  ret->Set(event_media_type_ == AUDIO_EVENT ? "audio" : "video",
           stats.release());

  return ret.Pass();
}

void StatsEventSubscriber::Reset() {
  DCHECK(thread_checker_.CalledOnValidThread());

  frame_stats_.clear();
  packet_stats_.clear();
  total_network_latency_ = base::TimeDelta();
  network_latency_datapoints_ = 0;
  total_e2e_latency_ = base::TimeDelta();
  e2e_latency_datapoints_ = 0;
  num_frames_dropped_by_encoder_ = 0;
  num_frames_late_ = 0;
  recent_frame_infos_.clear();
  packet_sent_times_.clear();
  start_time_ = clock_->NowTicks();
  last_response_received_time_ = base::TimeTicks();
  for (HistogramMap::iterator it = histograms_.begin(); it != histograms_.end();
       ++it) {
    it->second->Reset();
  }

  first_event_time_ = base::TimeTicks();
  last_event_time_ = base::TimeTicks();
}

// static
const char* StatsEventSubscriber::CastStatToString(CastStat stat) {
  switch (stat) {
    STAT_ENUM_TO_STRING(CAPTURE_FPS);
    STAT_ENUM_TO_STRING(ENCODE_FPS);
    STAT_ENUM_TO_STRING(DECODE_FPS);
    STAT_ENUM_TO_STRING(AVG_ENCODE_TIME_MS);
    STAT_ENUM_TO_STRING(AVG_PLAYOUT_DELAY_MS);
    STAT_ENUM_TO_STRING(AVG_NETWORK_LATENCY_MS);
    STAT_ENUM_TO_STRING(AVG_E2E_LATENCY_MS);
    STAT_ENUM_TO_STRING(ENCODE_KBPS);
    STAT_ENUM_TO_STRING(TRANSMISSION_KBPS);
    STAT_ENUM_TO_STRING(RETRANSMISSION_KBPS);
    STAT_ENUM_TO_STRING(PACKET_LOSS_FRACTION);
    STAT_ENUM_TO_STRING(MS_SINCE_LAST_RECEIVER_RESPONSE);
    STAT_ENUM_TO_STRING(NUM_FRAMES_CAPTURED);
    STAT_ENUM_TO_STRING(NUM_FRAMES_DROPPED_BY_ENCODER);
    STAT_ENUM_TO_STRING(NUM_FRAMES_LATE);
    STAT_ENUM_TO_STRING(NUM_PACKETS_SENT);
    STAT_ENUM_TO_STRING(NUM_PACKETS_RETRANSMITTED);
    STAT_ENUM_TO_STRING(NUM_PACKETS_RTX_REJECTED);
    STAT_ENUM_TO_STRING(FIRST_EVENT_TIME_MS);
    STAT_ENUM_TO_STRING(LAST_EVENT_TIME_MS);
    STAT_ENUM_TO_STRING(CAPTURE_LATENCY_MS_HISTO);
    STAT_ENUM_TO_STRING(ENCODE_LATENCY_MS_HISTO);
    STAT_ENUM_TO_STRING(PACKET_LATENCY_MS_HISTO);
    STAT_ENUM_TO_STRING(FRAME_LATENCY_MS_HISTO);
    STAT_ENUM_TO_STRING(PLAYOUT_DELAY_MS_HISTO);
  }
  NOTREACHED();
  return "";
}

const int kMaxLatencyBucketMs = 800;
const int kBucketWidthMs = 20;

void StatsEventSubscriber::InitHistograms() {
  histograms_[CAPTURE_LATENCY_MS_HISTO].reset(
      new SimpleHistogram(0, kMaxLatencyBucketMs, kBucketWidthMs));
  histograms_[ENCODE_LATENCY_MS_HISTO].reset(
      new SimpleHistogram(0, kMaxLatencyBucketMs, kBucketWidthMs));
  histograms_[PACKET_LATENCY_MS_HISTO].reset(
      new SimpleHistogram(0, kMaxLatencyBucketMs, kBucketWidthMs));
  histograms_[FRAME_LATENCY_MS_HISTO].reset(
      new SimpleHistogram(0, kMaxLatencyBucketMs, kBucketWidthMs));
  histograms_[PLAYOUT_DELAY_MS_HISTO].reset(
      new SimpleHistogram(0, kMaxLatencyBucketMs, kBucketWidthMs));
}

void StatsEventSubscriber::GetStatsInternal(StatsMap* stats_map) const {
  DCHECK(thread_checker_.CalledOnValidThread());

  stats_map->clear();

  base::TimeTicks end_time = clock_->NowTicks();

  PopulateFpsStat(
      end_time, FRAME_CAPTURE_BEGIN, CAPTURE_FPS, stats_map);
  PopulateFpsStat(
      end_time, FRAME_ENCODED, ENCODE_FPS, stats_map);
  PopulateFpsStat(
      end_time, FRAME_DECODED, DECODE_FPS, stats_map);
  PopulatePlayoutDelayStat(stats_map);
  PopulateFrameBitrateStat(end_time, stats_map);
  PopulatePacketBitrateStat(end_time,
                            PACKET_SENT_TO_NETWORK,
                            TRANSMISSION_KBPS,
                            stats_map);
  PopulatePacketBitrateStat(end_time,
                            PACKET_RETRANSMITTED,
                            RETRANSMISSION_KBPS,
                            stats_map);
  PopulatePacketLossPercentageStat(stats_map);
  PopulateFrameCountStat(FRAME_CAPTURE_END, NUM_FRAMES_CAPTURED, stats_map);
  PopulatePacketCountStat(PACKET_SENT_TO_NETWORK, NUM_PACKETS_SENT, stats_map);
  PopulatePacketCountStat(
      PACKET_RETRANSMITTED, NUM_PACKETS_RETRANSMITTED, stats_map);
  PopulatePacketCountStat(
      PACKET_RTX_REJECTED, NUM_PACKETS_RTX_REJECTED, stats_map);

  if (network_latency_datapoints_ > 0) {
    double avg_network_latency_ms =
        total_network_latency_.InMillisecondsF() /
        network_latency_datapoints_;
    stats_map->insert(
        std::make_pair(AVG_NETWORK_LATENCY_MS, avg_network_latency_ms));
  }

  if (e2e_latency_datapoints_ > 0) {
    double avg_e2e_latency_ms =
        total_e2e_latency_.InMillisecondsF() / e2e_latency_datapoints_;
    stats_map->insert(std::make_pair(AVG_E2E_LATENCY_MS, avg_e2e_latency_ms));
  }

  if (!last_response_received_time_.is_null()) {
    stats_map->insert(
        std::make_pair(MS_SINCE_LAST_RECEIVER_RESPONSE,
        (end_time - last_response_received_time_).InMillisecondsF()));
  }

  stats_map->insert(std::make_pair(NUM_FRAMES_DROPPED_BY_ENCODER,
                                   num_frames_dropped_by_encoder_));
  stats_map->insert(std::make_pair(NUM_FRAMES_LATE, num_frames_late_));
  if (!first_event_time_.is_null()) {
    stats_map->insert(std::make_pair(
        FIRST_EVENT_TIME_MS,
        (first_event_time_ - base::TimeTicks::UnixEpoch()).InMillisecondsF()));
  }
  if (!last_event_time_.is_null()) {
    stats_map->insert(std::make_pair(
        LAST_EVENT_TIME_MS,
        (last_event_time_ - base::TimeTicks::UnixEpoch()).InMillisecondsF()));
  }
}

bool StatsEventSubscriber::GetReceiverOffset(base::TimeDelta* offset) {
  base::TimeDelta receiver_offset_lower_bound;
  base::TimeDelta receiver_offset_upper_bound;
  if (!offset_estimator_->GetReceiverOffsetBounds(
          &receiver_offset_lower_bound, &receiver_offset_upper_bound)) {
    return false;
  }

  *offset = (receiver_offset_lower_bound + receiver_offset_upper_bound) / 2;
  return true;
}

void StatsEventSubscriber::MaybeInsertFrameInfo(RtpTimestamp rtp_timestamp,
                                                const FrameInfo& frame_info) {
  // No need to insert if |rtp_timestamp| is the smaller than every key in the
  // map as it is just going to get erased anyway.
  if (recent_frame_infos_.size() == kMaxFrameInfoMapSize &&
      rtp_timestamp < recent_frame_infos_.begin()->first) {
    return;
  }

  recent_frame_infos_.insert(std::make_pair(rtp_timestamp, frame_info));

  if (recent_frame_infos_.size() >= kMaxFrameInfoMapSize) {
    FrameInfoMap::iterator erase_it = recent_frame_infos_.begin();
    if (erase_it->second.encode_time.is_null())
      num_frames_dropped_by_encoder_++;
    recent_frame_infos_.erase(erase_it);
  }
}

void StatsEventSubscriber::RecordFrameCaptureTime(
    const FrameEvent& frame_event) {
  FrameInfo frame_info;
  frame_info.capture_time = frame_event.timestamp;
  MaybeInsertFrameInfo(frame_event.rtp_timestamp, frame_info);
}

void StatsEventSubscriber::RecordCaptureLatency(const FrameEvent& frame_event) {
  FrameInfoMap::iterator it =
      recent_frame_infos_.find(frame_event.rtp_timestamp);
  if (it == recent_frame_infos_.end())
    return;

  if (!it->second.capture_time.is_null()) {
    double capture_latency_ms =
        (it->second.capture_time - frame_event.timestamp).InMillisecondsF();
    histograms_[CAPTURE_LATENCY_MS_HISTO]->Add(capture_latency_ms);
  }

  it->second.capture_end_time = frame_event.timestamp;
}

void StatsEventSubscriber::RecordEncodeLatency(const FrameEvent& frame_event) {
  FrameInfoMap::iterator it =
      recent_frame_infos_.find(frame_event.rtp_timestamp);
  if (it == recent_frame_infos_.end()) {
    FrameInfo frame_info;
    frame_info.encode_time = frame_event.timestamp;
    MaybeInsertFrameInfo(frame_event.rtp_timestamp, frame_info);
    return;
  }

  if (!it->second.capture_end_time.is_null()) {
    double encode_latency_ms =
        (frame_event.timestamp - it->second.capture_end_time).InMillisecondsF();
    histograms_[ENCODE_LATENCY_MS_HISTO]->Add(encode_latency_ms);
  }

  it->second.encode_time = frame_event.timestamp;
}

void StatsEventSubscriber::RecordFrameTxLatency(const FrameEvent& frame_event) {
  FrameInfoMap::iterator it =
      recent_frame_infos_.find(frame_event.rtp_timestamp);
  if (it == recent_frame_infos_.end())
    return;

  if (it->second.encode_time.is_null())
    return;

  base::TimeDelta receiver_offset;
  if (!GetReceiverOffset(&receiver_offset))
    return;

  base::TimeTicks sender_time = frame_event.timestamp - receiver_offset;
  double frame_tx_latency_ms =
      (sender_time - it->second.encode_time).InMillisecondsF();
  histograms_[FRAME_LATENCY_MS_HISTO]->Add(frame_tx_latency_ms);
}

void StatsEventSubscriber::RecordE2ELatency(const FrameEvent& frame_event) {
  base::TimeDelta receiver_offset;
  if (!GetReceiverOffset(&receiver_offset))
    return;

  FrameInfoMap::iterator it =
      recent_frame_infos_.find(frame_event.rtp_timestamp);
  if (it == recent_frame_infos_.end())
    return;

  // Playout time is event time + playout delay.
  base::TimeTicks playout_time =
      frame_event.timestamp + frame_event.delay_delta - receiver_offset;
  total_e2e_latency_ += playout_time - it->second.capture_time;
  e2e_latency_datapoints_++;
}

void StatsEventSubscriber::UpdateLastResponseTime(
    base::TimeTicks receiver_time) {
  base::TimeDelta receiver_offset;
  if (!GetReceiverOffset(&receiver_offset))
    return;
  base::TimeTicks sender_time = receiver_time - receiver_offset;
  last_response_received_time_ = sender_time;
}

void StatsEventSubscriber::ErasePacketSentTime(
    const PacketEvent& packet_event) {
  std::pair<RtpTimestamp, uint16> key(
      std::make_pair(packet_event.rtp_timestamp, packet_event.packet_id));
  packet_sent_times_.erase(key);
}

void StatsEventSubscriber::RecordNetworkLatency(
    const PacketEvent& packet_event) {
  base::TimeDelta receiver_offset;
  if (!GetReceiverOffset(&receiver_offset))
    return;

  std::pair<RtpTimestamp, uint16> key(
      std::make_pair(packet_event.rtp_timestamp, packet_event.packet_id));
  PacketEventTimeMap::iterator it = packet_sent_times_.find(key);
  if (it == packet_sent_times_.end()) {
    std::pair<base::TimeTicks, CastLoggingEvent> value =
        std::make_pair(packet_event.timestamp, packet_event.type);
    packet_sent_times_.insert(std::make_pair(key, value));
    if (packet_sent_times_.size() > kMaxPacketEventTimeMapSize)
      packet_sent_times_.erase(packet_sent_times_.begin());
  } else {
    std::pair<base::TimeTicks, CastLoggingEvent> value = it->second;
    CastLoggingEvent recorded_type = value.second;
    bool match = false;
    base::TimeTicks packet_sent_time;
    base::TimeTicks packet_received_time;
    if (recorded_type == PACKET_SENT_TO_NETWORK &&
        packet_event.type == PACKET_RECEIVED) {
      packet_sent_time = value.first;
      packet_received_time = packet_event.timestamp;
      match = true;
    } else if (recorded_type == PACKET_RECEIVED &&
        packet_event.type == PACKET_SENT_TO_NETWORK) {
      packet_sent_time = packet_event.timestamp;
      packet_received_time = value.first;
      match = true;
    }
    if (match) {
      // Subtract by offset.
      packet_received_time -= receiver_offset;
      base::TimeDelta latency_delta = packet_received_time - packet_sent_time;

      total_network_latency_ += latency_delta;
      network_latency_datapoints_++;

      histograms_[PACKET_LATENCY_MS_HISTO]->Add(
          latency_delta.InMillisecondsF());

      packet_sent_times_.erase(it);
    }
  }
}

void StatsEventSubscriber::PopulateFpsStat(base::TimeTicks end_time,
                                           CastLoggingEvent event,
                                           CastStat stat,
                                           StatsMap* stats_map) const {
  FrameStatsMap::const_iterator it = frame_stats_.find(event);
  if (it != frame_stats_.end()) {
    double fps = 0.0;
    base::TimeDelta duration = (end_time - start_time_);
    int count = it->second.event_counter;
    if (duration > base::TimeDelta())
      fps = count / duration.InSecondsF();
    stats_map->insert(std::make_pair(stat, fps));
  }
}

void StatsEventSubscriber::PopulateFrameCountStat(CastLoggingEvent event,
                                                  CastStat stat,
                                                  StatsMap* stats_map) const {
  FrameStatsMap::const_iterator it = frame_stats_.find(event);
  if (it != frame_stats_.end()) {
    stats_map->insert(std::make_pair(stat, it->second.event_counter));
  }
}

void StatsEventSubscriber::PopulatePacketCountStat(CastLoggingEvent event,
                                                   CastStat stat,
                                                   StatsMap* stats_map) const {
  PacketStatsMap::const_iterator it = packet_stats_.find(event);
  if (it != packet_stats_.end()) {
    stats_map->insert(std::make_pair(stat, it->second.event_counter));
  }
}

void StatsEventSubscriber::PopulatePlayoutDelayStat(StatsMap* stats_map) const {
  FrameStatsMap::const_iterator it = frame_stats_.find(FRAME_PLAYOUT);
  if (it != frame_stats_.end()) {
    double avg_delay_ms = 0.0;
    base::TimeDelta sum_delay = it->second.sum_delay;
    int count = it->second.event_counter;
    if (count != 0)
      avg_delay_ms = sum_delay.InMillisecondsF() / count;
    stats_map->insert(std::make_pair(AVG_PLAYOUT_DELAY_MS, avg_delay_ms));
  }
}

void StatsEventSubscriber::PopulateFrameBitrateStat(base::TimeTicks end_time,
                                                    StatsMap* stats_map) const {
  FrameStatsMap::const_iterator it = frame_stats_.find(FRAME_ENCODED);
  if (it != frame_stats_.end()) {
    double kbps = 0.0;
    base::TimeDelta duration = end_time - start_time_;
    if (duration > base::TimeDelta()) {
      kbps = it->second.sum_size / duration.InMillisecondsF() * 8;
    }

    stats_map->insert(std::make_pair(ENCODE_KBPS, kbps));
  }
}

void StatsEventSubscriber::PopulatePacketBitrateStat(
    base::TimeTicks end_time,
    CastLoggingEvent event,
    CastStat stat,
    StatsMap* stats_map) const {
  PacketStatsMap::const_iterator it = packet_stats_.find(event);
  if (it != packet_stats_.end()) {
    double kbps = 0;
    base::TimeDelta duration = end_time - start_time_;
    if (duration > base::TimeDelta()) {
      kbps = it->second.sum_size / duration.InMillisecondsF() * 8;
    }

    stats_map->insert(std::make_pair(stat, kbps));
  }
}

void StatsEventSubscriber::PopulatePacketLossPercentageStat(
    StatsMap* stats_map) const {
  // We assume that retransmission means that the packet's previous
  // (re)transmission was lost.
  // This means the percentage of packet loss is
  // (# of retransmit events) / (# of transmit + retransmit events).
  PacketStatsMap::const_iterator sent_it =
      packet_stats_.find(PACKET_SENT_TO_NETWORK);
  if (sent_it == packet_stats_.end())
    return;
  PacketStatsMap::const_iterator retransmitted_it =
      packet_stats_.find(PACKET_RETRANSMITTED);
  int sent_count = sent_it->second.event_counter;
  int retransmitted_count = 0;
  if (retransmitted_it != packet_stats_.end())
    retransmitted_count = retransmitted_it->second.event_counter;
  double packet_loss_fraction = static_cast<double>(retransmitted_count) /
                                (sent_count + retransmitted_count);
  stats_map->insert(
      std::make_pair(PACKET_LOSS_FRACTION, packet_loss_fraction));
}

StatsEventSubscriber::FrameLogStats::FrameLogStats()
    : event_counter(0), sum_size(0) {}
StatsEventSubscriber::FrameLogStats::~FrameLogStats() {}

StatsEventSubscriber::PacketLogStats::PacketLogStats()
    : event_counter(0), sum_size(0) {}
StatsEventSubscriber::PacketLogStats::~PacketLogStats() {}

StatsEventSubscriber::FrameInfo::FrameInfo() : encoded(false) {
}
StatsEventSubscriber::FrameInfo::~FrameInfo() {
}

}  // namespace cast
}  // namespace media
