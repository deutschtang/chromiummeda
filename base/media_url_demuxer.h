// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_BASE_MEDIA_URL_DEMUXER_H_
#define MEDIA_BASE_MEDIA_URL_DEMUXER_H_

#include <stddef.h>

#include "base/callback.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "media/base/demuxer.h"
#include "url/gurl.h"

namespace base {
class SingleThreadTaskRunner;
}

namespace media {

// Class that saves a URL for later retrieval. To be used in conjunction with
// the MediaPlayerRenderer.
//
// Its primary purpose is to act as a dummy Demuxer, when there is no need
// for DemuxerStreams (e.g. in the MediaPlayerRenderer case). For the most part,
// its implementation of the Demuxer are NOPs that return the default values and
// fire any provided callbacks immediately.
//
// If Pipeline where to be refactored to use a DemuxerStreamProvider instead of
// a Demuxer, MediaUrlDemuxer should be refactored to inherit directly from
// DemuxerStreamProvider.
class MEDIA_EXPORT MediaUrlDemuxer : public Demuxer {
 public:
  MediaUrlDemuxer(
      const scoped_refptr<base::SingleThreadTaskRunner>& task_runner,
      const GURL& media_url,
      const GURL& first_party_for_cookies);
  ~MediaUrlDemuxer() override;

  // DemuxerStreamProvider interface.
  DemuxerStream* GetStream(DemuxerStream::Type type) override;
  MediaUrlParams GetMediaUrlParams() const override;
  DemuxerStreamProvider::Type GetType() const override;

  // Demuxer interface.
  std::string GetDisplayName() const override;
  void Initialize(DemuxerHost* host,
                  const PipelineStatusCB& status_cb,
                  bool enable_text_tracks) override;
  void StartWaitingForSeek(base::TimeDelta seek_time) override;
  void CancelPendingSeek(base::TimeDelta seek_time) override;
  void Seek(base::TimeDelta time, const PipelineStatusCB& status_cb) override;
  void Stop() override;
  void AbortPendingReads() override;
  base::TimeDelta GetStartTime() const override;
  base::Time GetTimelineOffset() const override;
  int64_t GetMemoryUsage() const override;
  void OnEnabledAudioTracksChanged(const std::vector<MediaTrack::Id>& track_ids,
                                   base::TimeDelta currTime) override;
  void OnSelectedVideoTrackChanged(const std::vector<MediaTrack::Id>& track_ids,
                                   base::TimeDelta currTime) override;

 private:
  MediaUrlParams params_;

  scoped_refptr<base::SingleThreadTaskRunner> task_runner_;

  DISALLOW_COPY_AND_ASSIGN(MediaUrlDemuxer);
};

}  // namespace media

#endif  // MEDIA_BASE_MEDIA_URL_DEMUXER_H_
