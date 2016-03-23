// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/blink/webmediaplayer_impl.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/callback_helpers.h"
#include "base/command_line.h"
#include "base/debug/alias.h"
#include "base/debug/crash_logging.h"
#include "base/debug/dump_without_crashing.h"
#include "base/metrics/histogram.h"
#include "base/rand_util.h"
#include "base/single_thread_task_runner.h"
#include "base/synchronization/waitable_event.h"
#include "base/task_runner_util.h"
#include "base/thread_task_runner_handle.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"
#include "cc/blink/web_layer_impl.h"
#include "cc/layers/video_layer.h"
#include "gpu/blink/webgraphicscontext3d_impl.h"
#include "media/audio/null_audio_sink.h"
#include "media/base/bind_to_current_loop.h"
#include "media/base/cdm_context.h"
#include "media/base/limits.h"
#include "media/base/media_log.h"
#include "media/base/media_switches.h"
#include "media/base/text_renderer.h"
#include "media/base/timestamp_constants.h"
#include "media/base/video_frame.h"
#include "media/blink/texttrack_impl.h"
#include "media/blink/webaudiosourceprovider_impl.h"
#include "media/blink/webcontentdecryptionmodule_impl.h"
#include "media/blink/webinbandtexttrack_impl.h"
#include "media/blink/webmediaplayer_delegate.h"
#include "media/blink/webmediaplayer_util.h"
#include "media/blink/webmediasource_impl.h"
#include "media/filters/chunk_demuxer.h"
#include "media/filters/ffmpeg_demuxer.h"
#include "third_party/WebKit/public/platform/WebEncryptedMediaTypes.h"
#include "third_party/WebKit/public/platform/WebMediaPlayerClient.h"
#include "third_party/WebKit/public/platform/WebMediaPlayerEncryptedMediaClient.h"
#include "third_party/WebKit/public/platform/WebMediaSource.h"
#include "third_party/WebKit/public/platform/WebRect.h"
#include "third_party/WebKit/public/platform/WebSecurityOrigin.h"
#include "third_party/WebKit/public/platform/WebSize.h"
#include "third_party/WebKit/public/platform/WebString.h"
#include "third_party/WebKit/public/platform/WebURL.h"
#include "third_party/WebKit/public/web/WebDocument.h"
#include "third_party/WebKit/public/web/WebFrame.h"
#include "third_party/WebKit/public/web/WebLocalFrame.h"
#include "third_party/WebKit/public/web/WebView.h"

using blink::WebCanvas;
using blink::WebMediaPlayer;
using blink::WebRect;
using blink::WebSize;
using blink::WebString;
using gpu::gles2::GLES2Interface;

#define STATIC_ASSERT_ENUM(a, b)                            \
  static_assert(static_cast<int>(a) == static_cast<int>(b), \
                "mismatching enums: " #a)

namespace media {

namespace {

// Limits the range of playback rate.
//
// TODO(kylep): Revisit these.
//
// Vista has substantially lower performance than XP or Windows7.  If you speed
// up a video too much, it can't keep up, and rendering stops updating except on
// the time bar. For really high speeds, audio becomes a bottleneck and we just
// use up the data we have, which may not achieve the speed requested, but will
// not crash the tab.
//
// A very slow speed, ie 0.00000001x, causes the machine to lock up. (It seems
// like a busy loop). It gets unresponsive, although its not completely dead.
//
// Also our timers are not very accurate (especially for ogg), which becomes
// evident at low speeds and on Vista. Since other speeds are risky and outside
// the norms, we think 1/16x to 16x is a safe and useful range for now.
const double kMinRate = 0.0625;
const double kMaxRate = 16.0;

void SetSinkIdOnMediaThread(scoped_refptr<WebAudioSourceProviderImpl> sink,
                            const std::string& device_id,
                            const url::Origin& security_origin,
                            const SwitchOutputDeviceCB& callback) {
  if (sink->GetOutputDevice()) {
    sink->GetOutputDevice()->SwitchOutputDevice(device_id, security_origin,
                                                callback);
  } else {
    callback.Run(OUTPUT_DEVICE_STATUS_ERROR_INTERNAL);
  }
}

bool IsSuspendUponHiddenEnabled() {
#if !defined(OS_ANDROID)
  // Suspend/Resume is only enabled by default on Android.
  return base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kEnableMediaSuspend);
#else
  return !base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kDisableMediaSuspend);
#endif
}

}  // namespace

class BufferedDataSourceHostImpl;

STATIC_ASSERT_ENUM(WebMediaPlayer::CORSModeUnspecified,
                   UrlData::CORS_UNSPECIFIED);
STATIC_ASSERT_ENUM(WebMediaPlayer::CORSModeAnonymous, UrlData::CORS_ANONYMOUS);
STATIC_ASSERT_ENUM(WebMediaPlayer::CORSModeUseCredentials,
                   UrlData::CORS_USE_CREDENTIALS);

#define BIND_TO_RENDER_LOOP(function) \
  (DCHECK(main_task_runner_->BelongsToCurrentThread()), \
  BindToCurrentLoop(base::Bind(function, AsWeakPtr())))

#define BIND_TO_RENDER_LOOP1(function, arg1) \
  (DCHECK(main_task_runner_->BelongsToCurrentThread()), \
  BindToCurrentLoop(base::Bind(function, AsWeakPtr(), arg1)))

WebMediaPlayerImpl::WebMediaPlayerImpl(
    blink::WebLocalFrame* frame,
    blink::WebMediaPlayerClient* client,
    blink::WebMediaPlayerEncryptedMediaClient* encrypted_client,
    base::WeakPtr<WebMediaPlayerDelegate> delegate,
    scoped_ptr<RendererFactory> renderer_factory,
    linked_ptr<UrlIndex> url_index,
    const WebMediaPlayerParams& params)
    : frame_(frame),
      network_state_(WebMediaPlayer::NetworkStateEmpty),
      ready_state_(WebMediaPlayer::ReadyStateHaveNothing),
      preload_(BufferedDataSource::AUTO),
      buffering_strategy_(
          BufferedDataSourceInterface::BUFFERING_STRATEGY_NORMAL),
      main_task_runner_(base::ThreadTaskRunnerHandle::Get()),
      media_task_runner_(params.media_task_runner()),
      worker_task_runner_(params.worker_task_runner()),
      media_log_(params.media_log()),
      pipeline_(media_task_runner_, media_log_.get()),
      pipeline_controller_(
          &pipeline_,
          base::Bind(&WebMediaPlayerImpl::CreateRenderer,
                     base::Unretained(this)),
          base::Bind(&WebMediaPlayerImpl::OnPipelineSeeked, AsWeakPtr()),
          base::Bind(&WebMediaPlayerImpl::OnPipelineSuspended, AsWeakPtr()),
          base::Bind(&WebMediaPlayerImpl::OnPipelineResumed, AsWeakPtr()),
          base::Bind(&WebMediaPlayerImpl::OnPipelineError, AsWeakPtr())),
      load_type_(LoadTypeURL),
      opaque_(false),
      playback_rate_(0.0),
      paused_(true),
      seeking_(false),
      pending_suspend_resume_cycle_(false),
      ended_(false),
      should_notify_time_changed_(false),
      fullscreen_(false),
      decoder_requires_restart_for_fullscreen_(false),
      client_(client),
      encrypted_client_(encrypted_client),
      delegate_(delegate),
      delegate_id_(0),
      defer_load_cb_(params.defer_load_cb()),
      context_3d_cb_(params.context_3d_cb()),
      adjust_allocated_memory_cb_(params.adjust_allocated_memory_cb()),
      last_reported_memory_usage_(0),
      supports_save_(true),
      chunk_demuxer_(NULL),
      url_index_(url_index),
      // Threaded compositing isn't enabled universally yet.
      compositor_task_runner_(
          params.compositor_task_runner()
              ? params.compositor_task_runner()
              : base::MessageLoop::current()->task_runner()),
      compositor_(new VideoFrameCompositor(
          compositor_task_runner_,
          BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnNaturalSizeChanged),
          BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnOpacityChanged))),
      is_cdm_attached_(false),
#if defined(OS_ANDROID)  // WMPI_CAST
      cast_impl_(this, client_, params.context_3d_cb()),
#endif
      volume_(1.0),
      volume_multiplier_(1.0),
      renderer_factory_(std::move(renderer_factory)),
      surface_manager_(params.surface_manager()),
      suppress_destruction_errors_(false) {
  DCHECK(!adjust_allocated_memory_cb_.is_null());
  DCHECK(renderer_factory_);

  if (delegate_)
    delegate_id_ = delegate_->AddObserver(this);

  media_log_->AddEvent(
      media_log_->CreateEvent(MediaLogEvent::WEBMEDIAPLAYER_CREATED));

  if (params.initial_cdm()) {
    SetCdm(base::Bind(&IgnoreCdmAttached),
           ToWebContentDecryptionModuleImpl(params.initial_cdm())
               ->GetCdmContext());
  }

  // TODO(xhwang): When we use an external Renderer, many methods won't work,
  // e.g. GetCurrentFrameFromCompositor(). See http://crbug.com/434861

  // Use the null sink if no sink was provided.
  audio_source_provider_ = new WebAudioSourceProviderImpl(
      params.audio_renderer_sink().get()
          ? params.audio_renderer_sink()
          : new NullAudioSink(media_task_runner_));
}

WebMediaPlayerImpl::~WebMediaPlayerImpl() {
  client_->setWebLayer(NULL);

  DCHECK(main_task_runner_->BelongsToCurrentThread());

  if (delegate_) {
    delegate_->PlayerGone(delegate_id_);
    delegate_->RemoveObserver(delegate_id_);
  }

  // Abort any pending IO so stopping the pipeline doesn't get blocked.
  suppress_destruction_errors_ = true;
  if (data_source_)
    data_source_->Abort();
  if (chunk_demuxer_) {
    chunk_demuxer_->Shutdown();
    chunk_demuxer_ = nullptr;
  }

  renderer_factory_.reset();

  // Make sure to kill the pipeline so there's no more media threads running.
  // Note: stopping the pipeline might block for a long time.
  base::WaitableEvent waiter(false, false);
  pipeline_.Stop(
      base::Bind(&base::WaitableEvent::Signal, base::Unretained(&waiter)));
  waiter.Wait();

  if (last_reported_memory_usage_)
    adjust_allocated_memory_cb_.Run(-last_reported_memory_usage_);

  compositor_task_runner_->DeleteSoon(FROM_HERE, compositor_);

  media_log_->AddEvent(
      media_log_->CreateEvent(MediaLogEvent::WEBMEDIAPLAYER_DESTROYED));
}

void WebMediaPlayerImpl::load(LoadType load_type, const blink::WebURL& url,
                              CORSMode cors_mode) {
  DVLOG(1) << __FUNCTION__ << "(" << load_type << ", " << url << ", "
           << cors_mode << ")";
  if (!defer_load_cb_.is_null()) {
    defer_load_cb_.Run(base::Bind(
        &WebMediaPlayerImpl::DoLoad, AsWeakPtr(), load_type, url, cors_mode));
    return;
  }
  DoLoad(load_type, url, cors_mode);
}

void WebMediaPlayerImpl::enteredFullscreen() {
  fullscreen_ = true;
  if (decoder_requires_restart_for_fullscreen_)
    ScheduleRestart();
}

void WebMediaPlayerImpl::exitedFullscreen() {
  fullscreen_ = false;
  if (decoder_requires_restart_for_fullscreen_)
    ScheduleRestart();
}

void WebMediaPlayerImpl::DoLoad(LoadType load_type,
                                const blink::WebURL& url,
                                CORSMode cors_mode) {
  DVLOG(1) << __FUNCTION__;
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  GURL gurl(url);
  ReportMetrics(load_type, gurl, frame_->document().getSecurityOrigin());

  // Set subresource URL for crash reporting.
  base::debug::SetCrashKeyValue("subresource_url", gurl.spec());

  load_type_ = load_type;

  SetNetworkState(WebMediaPlayer::NetworkStateLoading);
  SetReadyState(WebMediaPlayer::ReadyStateHaveNothing);
  media_log_->AddEvent(media_log_->CreateLoadEvent(url.string().utf8()));

  // Media source pipelines can start immediately.
  if (load_type == LoadTypeMediaSource) {
    supports_save_ = false;
    StartPipeline();
    return;
  }

  // TODO(hubbe): This experiment is temporary and should be removed once
  // we have enough data to support the primacy of the new media cache.
  // See http://crbug.com/514719 for details.
  // Otherwise it's a regular request which requires resolving the URL first.
  if (base::FeatureList::IsEnabled(kUseNewMediaCache)) {
    // Remove this when MultiBufferDataSource becomes default.
    LOG(WARNING) << "Using MultibufferDataSource";
    data_source_.reset(new MultibufferDataSource(
        url, static_cast<UrlData::CORSMode>(cors_mode), main_task_runner_,
        url_index_, frame_, media_log_.get(), &buffered_data_source_host_,
        base::Bind(&WebMediaPlayerImpl::NotifyDownloading, AsWeakPtr())));
  } else {
    data_source_.reset(new BufferedDataSource(
        url, static_cast<BufferedResourceLoader::CORSMode>(cors_mode),
        main_task_runner_, frame_, media_log_.get(),
        &buffered_data_source_host_,
        base::Bind(&WebMediaPlayerImpl::NotifyDownloading, AsWeakPtr())));
  }
  data_source_->SetPreload(preload_);
  data_source_->SetBufferingStrategy(buffering_strategy_);
  data_source_->Initialize(
      base::Bind(&WebMediaPlayerImpl::DataSourceInitialized, AsWeakPtr()));

#if defined(OS_ANDROID)  // WMPI_CAST
  cast_impl_.Initialize(url, frame_, delegate_id_);
#endif
}

void WebMediaPlayerImpl::play() {
  DVLOG(1) << __FUNCTION__;
  DCHECK(main_task_runner_->BelongsToCurrentThread());

#if defined(OS_ANDROID)  // WMPI_CAST
  if (isRemote()) {
    cast_impl_.play();
    return;
  }
#endif

  const bool was_paused = paused_;
  paused_ = false;
  pipeline_.SetPlaybackRate(playback_rate_);

  if (data_source_)
    data_source_->MediaIsPlaying();

  media_log_->AddEvent(media_log_->CreateEvent(MediaLogEvent::PLAY));

  if (playback_rate_ > 0 && was_paused) {
    NotifyPlaybackStarted();

    // Resume the player if allowed. We always call Resume() in case there is a
    // pending suspend that should be aborted. If the pipeline is not suspended,
    // Resume() will have no effect.
    if (IsAutomaticResumeAllowed())
      pipeline_controller_.Resume();
  }
}

void WebMediaPlayerImpl::pause() {
  DVLOG(1) << __FUNCTION__;
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  const bool was_already_paused = paused_ || playback_rate_ == 0;
  paused_ = true;

#if defined(OS_ANDROID)  // WMPI_CAST
  if (isRemote()) {
    cast_impl_.pause();
    return;
  }
#endif

  pipeline_.SetPlaybackRate(0.0);

  // pause() may be called after playback has ended and the HTMLMediaElement
  // requires that currentTime() == duration() after ending.  We want to ensure
  // |paused_time_| matches currentTime() in this case or a future seek() may
  // incorrectly discard what it thinks is a seek to the existing time.
  paused_time_ =
      ended_ ? pipeline_.GetMediaDuration() : pipeline_.GetMediaTime();

  media_log_->AddEvent(media_log_->CreateEvent(MediaLogEvent::PAUSE));

  if (!was_already_paused)
    NotifyPlaybackPaused();
}

bool WebMediaPlayerImpl::supportsSave() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  return supports_save_;
}

void WebMediaPlayerImpl::seek(double seconds) {
  DVLOG(1) << __FUNCTION__ << "(" << seconds << "s)";
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  DoSeek(base::TimeDelta::FromSecondsD(seconds), true);
}

void WebMediaPlayerImpl::DoSeek(base::TimeDelta time, bool time_updated) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  ended_ = false;

#if defined(OS_ANDROID)  // WMPI_CAST
  if (isRemote()) {
    cast_impl_.seek(time);
    return;
  }
#endif

  ReadyState old_state = ready_state_;
  if (ready_state_ > WebMediaPlayer::ReadyStateHaveMetadata)
    SetReadyState(WebMediaPlayer::ReadyStateHaveMetadata);

  // When paused, we know exactly what the current time is and can elide seeks
  // to it. However, there are two cases that are not elided:
  //   1) When the pipeline state is not stable.
  //      In this case we just let |pipeline_controller_| decide what to do, as
  //      it has complete information.
  //   2) For MSE.
  //      Because the buffers may have changed between seeks, MSE seeks are
  //      never elided.
  if (paused_ && pipeline_controller_.IsStable() && paused_time_ == time &&
      !chunk_demuxer_) {
    // If the ready state was high enough before, we can indicate that the seek
    // completed just by restoring it. Otherwise we will just wait for the real
    // ready state change to eventually happen.
    if (old_state == ReadyStateHaveEnoughData) {
      main_task_runner_->PostTask(
          FROM_HERE,
          base::Bind(&WebMediaPlayerImpl::OnPipelineBufferingStateChanged,
                     AsWeakPtr(), BUFFERING_HAVE_ENOUGH));
    }
    return;
  }

  seeking_ = true;
  seek_time_ = time;
  if (paused_)
    paused_time_ = time;
  pipeline_controller_.Seek(time, time_updated);

  // Resume the pipeline if allowed so that the correct frame is displayed. We
  // always call Resume() in case there is a pending suspend that should be
  // aborted. If the pipeline is not suspended, Resume() will have no effect.
  if (IsAutomaticResumeAllowed())
    pipeline_controller_.Resume();
}

void WebMediaPlayerImpl::setRate(double rate) {
  DVLOG(1) << __FUNCTION__ << "(" << rate << ")";
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  // TODO(kylep): Remove when support for negatives is added. Also, modify the
  // following checks so rewind uses reasonable values also.
  if (rate < 0.0)
    return;

  // Limit rates to reasonable values by clamping.
  if (rate != 0.0) {
    if (rate < kMinRate)
      rate = kMinRate;
    else if (rate > kMaxRate)
      rate = kMaxRate;
    if (playback_rate_ == 0 && !paused_)
      NotifyPlaybackStarted();
  } else if (playback_rate_ != 0 && !paused_) {
    NotifyPlaybackPaused();
  }

  playback_rate_ = rate;
  if (!paused_) {
    pipeline_.SetPlaybackRate(rate);
    if (data_source_)
      data_source_->MediaPlaybackRateChanged(rate);
  }
}

void WebMediaPlayerImpl::setVolume(double volume) {
  DVLOG(1) << __FUNCTION__ << "(" << volume << ")";
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  volume_ = volume;
  pipeline_.SetVolume(volume_ * volume_multiplier_);
}

void WebMediaPlayerImpl::setSinkId(
    const blink::WebString& sink_id,
    const blink::WebSecurityOrigin& security_origin,
    blink::WebSetSinkIdCallbacks* web_callback) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  DVLOG(1) << __FUNCTION__;

  media::SwitchOutputDeviceCB callback =
      media::ConvertToSwitchOutputDeviceCB(web_callback);
  media_task_runner_->PostTask(
      FROM_HERE,
      base::Bind(&SetSinkIdOnMediaThread, audio_source_provider_,
                 sink_id.utf8(), static_cast<url::Origin>(security_origin),
                 callback));
}

STATIC_ASSERT_ENUM(WebMediaPlayer::PreloadNone, BufferedDataSource::NONE);
STATIC_ASSERT_ENUM(WebMediaPlayer::PreloadMetaData,
                   BufferedDataSource::METADATA);
STATIC_ASSERT_ENUM(WebMediaPlayer::PreloadAuto, BufferedDataSource::AUTO);

void WebMediaPlayerImpl::setPreload(WebMediaPlayer::Preload preload) {
  DVLOG(1) << __FUNCTION__ << "(" << preload << ")";
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  preload_ = static_cast<BufferedDataSource::Preload>(preload);
  if (data_source_)
    data_source_->SetPreload(preload_);
}

STATIC_ASSERT_ENUM(WebMediaPlayer::BufferingStrategy::Normal,
                   BufferedDataSource::BUFFERING_STRATEGY_NORMAL);
STATIC_ASSERT_ENUM(WebMediaPlayer::BufferingStrategy::Aggressive,
                   BufferedDataSource::BUFFERING_STRATEGY_AGGRESSIVE);

void WebMediaPlayerImpl::setBufferingStrategy(
    WebMediaPlayer::BufferingStrategy buffering_strategy) {
  DVLOG(1) << __FUNCTION__;
  DCHECK(main_task_runner_->BelongsToCurrentThread());

#if defined(OS_ANDROID)
  // We disallow aggressive buffering on Android since it matches the behavior
  // of the platform media player and may have data usage penalties.
  // TODO(dalecurtis, hubbe): We should probably stop using "pause-and-buffer"
  // everywhere. See http://crbug.com/594669 for more details.
  buffering_strategy_ = BufferedDataSource::BUFFERING_STRATEGY_NORMAL;
#else
  buffering_strategy_ =
      static_cast<BufferedDataSource::BufferingStrategy>(buffering_strategy);
#endif

  if (data_source_)
    data_source_->SetBufferingStrategy(buffering_strategy_);
}

bool WebMediaPlayerImpl::hasVideo() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  return pipeline_metadata_.has_video;
}

bool WebMediaPlayerImpl::hasAudio() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  return pipeline_metadata_.has_audio;
}

blink::WebSize WebMediaPlayerImpl::naturalSize() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  return blink::WebSize(pipeline_metadata_.natural_size);
}

bool WebMediaPlayerImpl::paused() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

#if defined(OS_ANDROID)  // WMPI_CAST
  if (isRemote())
    return cast_impl_.paused();
#endif
  return pipeline_.GetPlaybackRate() == 0.0f;
}

bool WebMediaPlayerImpl::seeking() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  if (ready_state_ == WebMediaPlayer::ReadyStateHaveNothing)
    return false;

  return seeking_;
}

double WebMediaPlayerImpl::duration() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  if (ready_state_ == WebMediaPlayer::ReadyStateHaveNothing)
    return std::numeric_limits<double>::quiet_NaN();

  return GetPipelineDuration();
}

double WebMediaPlayerImpl::timelineOffset() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  if (pipeline_metadata_.timeline_offset.is_null())
    return std::numeric_limits<double>::quiet_NaN();

  return pipeline_metadata_.timeline_offset.ToJsTime();
}

double WebMediaPlayerImpl::currentTime() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  DCHECK_NE(ready_state_, WebMediaPlayer::ReadyStateHaveNothing);

  // TODO(scherkus): Replace with an explicit ended signal to HTMLMediaElement,
  // see http://crbug.com/409280
  if (ended_)
    return duration();

  if (seeking())
    return seek_time_.InSecondsF();

#if defined(OS_ANDROID)  // WMPI_CAST
  if (isRemote())
    return cast_impl_.currentTime();
#endif

  if (paused_)
    return paused_time_.InSecondsF();

  return pipeline_.GetMediaTime().InSecondsF();
}

WebMediaPlayer::NetworkState WebMediaPlayerImpl::getNetworkState() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  return network_state_;
}

WebMediaPlayer::ReadyState WebMediaPlayerImpl::getReadyState() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  return ready_state_;
}

blink::WebTimeRanges WebMediaPlayerImpl::buffered() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  Ranges<base::TimeDelta> buffered_time_ranges =
      pipeline_.GetBufferedTimeRanges();

  const base::TimeDelta duration = pipeline_.GetMediaDuration();
  if (duration != kInfiniteDuration()) {
    buffered_data_source_host_.AddBufferedTimeRanges(
        &buffered_time_ranges, duration);
  }
  return ConvertToWebTimeRanges(buffered_time_ranges);
}

blink::WebTimeRanges WebMediaPlayerImpl::seekable() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  if (ready_state_ < WebMediaPlayer::ReadyStateHaveMetadata)
    return blink::WebTimeRanges();

  const double seekable_end = duration();

  // Allow a special exception for seeks to zero for streaming sources with a
  // finite duration; this allows looping to work.
  const bool allow_seek_to_zero = data_source_ && data_source_->IsStreaming() &&
                                  std::isfinite(seekable_end);

  // TODO(dalecurtis): Technically this allows seeking on media which return an
  // infinite duration so long as DataSource::IsStreaming() is false.  While not
  // expected, disabling this breaks semi-live players, http://crbug.com/427412.
  const blink::WebTimeRange seekable_range(
      0.0, allow_seek_to_zero ? 0.0 : seekable_end);
  return blink::WebTimeRanges(&seekable_range, 1);
}

bool WebMediaPlayerImpl::didLoadingProgress() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  bool pipeline_progress = pipeline_.DidLoadingProgress();
  bool data_progress = buffered_data_source_host_.DidLoadingProgress();
  return pipeline_progress || data_progress;
}

void WebMediaPlayerImpl::paint(blink::WebCanvas* canvas,
                               const blink::WebRect& rect,
                               unsigned char alpha,
                               SkXfermode::Mode mode) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  TRACE_EVENT0("media", "WebMediaPlayerImpl:paint");

  if (is_cdm_attached_)
    return;

  scoped_refptr<VideoFrame> video_frame = GetCurrentFrameFromCompositor();

  gfx::Rect gfx_rect(rect);
  Context3D context_3d;
  if (video_frame.get() && video_frame->HasTextures()) {
    if (!context_3d_cb_.is_null())
      context_3d = context_3d_cb_.Run();
    // GPU Process crashed.
    if (!context_3d.gl)
      return;
  }
  skcanvas_video_renderer_.Paint(video_frame, canvas, gfx::RectF(gfx_rect),
                                 alpha, mode, pipeline_metadata_.video_rotation,
                                 context_3d);
}

bool WebMediaPlayerImpl::hasSingleSecurityOrigin() const {
  if (data_source_)
    return data_source_->HasSingleOrigin();
  return true;
}

bool WebMediaPlayerImpl::didPassCORSAccessCheck() const {
  if (data_source_)
    return data_source_->DidPassCORSAccessCheck();
  return false;
}

double WebMediaPlayerImpl::mediaTimeForTimeValue(double timeValue) const {
  return base::TimeDelta::FromSecondsD(timeValue).InSecondsF();
}

unsigned WebMediaPlayerImpl::decodedFrameCount() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  PipelineStatistics stats = pipeline_.GetStatistics();
  return stats.video_frames_decoded;
}

unsigned WebMediaPlayerImpl::droppedFrameCount() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  PipelineStatistics stats = pipeline_.GetStatistics();
  return stats.video_frames_dropped;
}

size_t WebMediaPlayerImpl::audioDecodedByteCount() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  PipelineStatistics stats = pipeline_.GetStatistics();
  return stats.audio_bytes_decoded;
}

size_t WebMediaPlayerImpl::videoDecodedByteCount() const {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  PipelineStatistics stats = pipeline_.GetStatistics();
  return stats.video_bytes_decoded;
}

bool WebMediaPlayerImpl::copyVideoTextureToPlatformTexture(
    blink::WebGraphicsContext3D* web_graphics_context,
    unsigned int texture,
    unsigned int internal_format,
    unsigned int type,
    bool premultiply_alpha,
    bool flip_y) {
  TRACE_EVENT0("media", "WebMediaPlayerImpl:copyVideoTextureToPlatformTexture");

  scoped_refptr<VideoFrame> video_frame = GetCurrentFrameFromCompositor();

  if (!video_frame.get() || !video_frame->HasTextures() ||
      media::VideoFrame::NumPlanes(video_frame->format()) != 1) {
    return false;
  }

  // TODO(dshwang): need more elegant way to convert WebGraphicsContext3D to
  // GLES2Interface.
  gpu::gles2::GLES2Interface* gl =
      static_cast<gpu_blink::WebGraphicsContext3DImpl*>(web_graphics_context)
          ->GetGLInterface();
  SkCanvasVideoRenderer::CopyVideoFrameSingleTextureToGLTexture(
      gl, video_frame.get(), texture, internal_format, type, premultiply_alpha,
      flip_y);
  return true;
}

void WebMediaPlayerImpl::setContentDecryptionModule(
    blink::WebContentDecryptionModule* cdm,
    blink::WebContentDecryptionModuleResult result) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  // Once the CDM is set it can't be cleared as there may be frames being
  // decrypted on other threads. So fail this request.
  // http://crbug.com/462365#c7.
  if (!cdm) {
    result.completeWithError(
        blink::WebContentDecryptionModuleExceptionInvalidStateError, 0,
        "The existing MediaKeys object cannot be removed at this time.");
    return;
  }

  // Create a local copy of |result| to avoid problems with the callback
  // getting passed to the media thread and causing |result| to be destructed
  // on the wrong thread in some failure conditions. Blink should prevent
  // multiple simultaneous calls.
  DCHECK(!set_cdm_result_);
  set_cdm_result_.reset(new blink::WebContentDecryptionModuleResult(result));

  SetCdm(BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnCdmAttached),
         ToWebContentDecryptionModuleImpl(cdm)->GetCdmContext());
}

void WebMediaPlayerImpl::OnEncryptedMediaInitData(
    EmeInitDataType init_data_type,
    const std::vector<uint8_t>& init_data) {
  DCHECK(init_data_type != EmeInitDataType::UNKNOWN);

  // TODO(xhwang): Update this UMA name. https://crbug.com/589251
  UMA_HISTOGRAM_COUNTS("Media.EME.NeedKey", 1);

  encrypted_client_->encrypted(
      ConvertToWebInitDataType(init_data_type), init_data.data(),
      base::saturated_cast<unsigned int>(init_data.size()));
}

void WebMediaPlayerImpl::OnFFmpegMediaTracksUpdated(
    scoped_ptr<MediaTracks> tracks) {
  // For MSE/chunk_demuxer case the media track updates are handled by
  // WebSourceBufferImpl.
  DCHECK(demuxer_.get());
  DCHECK(!chunk_demuxer_);
}

void WebMediaPlayerImpl::OnWaitingForDecryptionKey() {
  encrypted_client_->didBlockPlaybackWaitingForKey();

  // TODO(jrummell): didResumePlaybackBlockedForKey() should only be called
  // when a key has been successfully added (e.g. OnSessionKeysChange() with
  // |has_additional_usable_key| = true). http://crbug.com/461903
  encrypted_client_->didResumePlaybackBlockedForKey();
}

void WebMediaPlayerImpl::SetCdm(const CdmAttachedCB& cdm_attached_cb,
                                CdmContext* cdm_context) {
  if (!cdm_context) {
    cdm_attached_cb.Run(false);
    return;
  }

  // If CDM initialization succeeded, tell the pipeline about it.
  pipeline_.SetCdm(cdm_context, cdm_attached_cb);
}

void WebMediaPlayerImpl::OnCdmAttached(bool success) {
  if (success) {
    set_cdm_result_->complete();
    set_cdm_result_.reset();
    is_cdm_attached_ = true;
    return;
  }

  set_cdm_result_->completeWithError(
      blink::WebContentDecryptionModuleExceptionNotSupportedError, 0,
      "Unable to set MediaKeys object");
  set_cdm_result_.reset();
}

void WebMediaPlayerImpl::OnPipelineSeeked(bool time_updated) {
  seeking_ = false;
  seek_time_ = base::TimeDelta();
  if (paused_) {
#if defined(OS_ANDROID)  // WMPI_CAST
    if (isRemote()) {
      paused_time_ = base::TimeDelta::FromSecondsD(cast_impl_.currentTime());
    } else {
      paused_time_ = pipeline_.GetMediaTime();
    }
#else
    paused_time_ = pipeline_.GetMediaTime();
#endif
  }
  if (time_updated)
    should_notify_time_changed_ = true;
}

void WebMediaPlayerImpl::OnPipelineSuspended() {
#if defined(OS_ANDROID)
  if (isRemote()) {
    if (delegate_)
      delegate_->PlayerGone(delegate_id_);
    scoped_refptr<VideoFrame> frame = cast_impl_.GetCastingBanner();
    if (frame) {
      compositor_->PaintFrameUsingOldRenderingPath(frame);
    }
  }
#endif

  memory_usage_reporting_timer_.Stop();
  ReportMemoryUsage();

  // If we're not in an aggressive buffering state, tell the data source we have
  // enough data so that it may release the connection.
  if (buffering_strategy_ !=
      BufferedDataSource::BUFFERING_STRATEGY_AGGRESSIVE) {
    if (data_source_)
      data_source_->OnBufferingHaveEnough(true);
  }

  if (pending_suspend_resume_cycle_) {
    pending_suspend_resume_cycle_ = false;
    pipeline_controller_.Resume();
    return;
  }
}

void WebMediaPlayerImpl::OnPipelineResumed() {
  if (playback_rate_ > 0 && !paused_) {
    NotifyPlaybackStarted();
  } else if (!playback_rate_ || paused_ || ended_) {
    // Resend our paused notification so the pipeline is considered for idle
    // resource reclamation; duplicate pause notifications are ignored.
    NotifyPlaybackPaused();
  }
}

void WebMediaPlayerImpl::OnPipelineEnded() {
  DVLOG(1) << __FUNCTION__;
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  // Ignore state changes until we've completed all outstanding operations.
  if (!pipeline_controller_.IsStable())
    return;

  ended_ = true;
  client_->timeChanged();
}

void WebMediaPlayerImpl::OnPipelineError(PipelineStatus error) {
  DVLOG(1) << __FUNCTION__;
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  DCHECK_NE(error, PIPELINE_OK);

  if (suppress_destruction_errors_)
    return;

  // Release the delegate for player errors; this drops the media session and
  // avoids idle suspension from ticking.
  if (delegate_)
    delegate_->PlayerGone(delegate_id_);

#if defined(OS_ANDROID)
  // For 10% of pipeline decode failures log the playback URL. The URL is set
  // as the crash-key 'subresource_url' during DoLoad().
  //
  // TODO(dalecurtis): This is temporary to track down higher than average
  // decode failure rates for video-only content. See http://crbug.com/595076.
  if (base::RandDouble() <= 0.1 && error == PIPELINE_ERROR_DECODE)
    base::debug::DumpWithoutCrashing();
#endif

  media_log_->AddEvent(media_log_->CreatePipelineErrorEvent(error));

  if (ready_state_ == WebMediaPlayer::ReadyStateHaveNothing) {
    // Any error that occurs before reaching ReadyStateHaveMetadata should
    // be considered a format error.
    SetNetworkState(WebMediaPlayer::NetworkStateFormatError);
    return;
  }

  SetNetworkState(PipelineErrorToNetworkState(error));
}

void WebMediaPlayerImpl::OnPipelineMetadata(
    PipelineMetadata metadata) {
  DVLOG(1) << __FUNCTION__;

  pipeline_metadata_ = metadata;

  UMA_HISTOGRAM_ENUMERATION("Media.VideoRotation", metadata.video_rotation,
                            VIDEO_ROTATION_MAX + 1);
  SetReadyState(WebMediaPlayer::ReadyStateHaveMetadata);

  if (hasVideo()) {
    DCHECK(!video_weblayer_);
    scoped_refptr<cc::VideoLayer> layer =
        cc::VideoLayer::Create(compositor_, pipeline_metadata_.video_rotation);

    if (pipeline_metadata_.video_rotation == VIDEO_ROTATION_90 ||
        pipeline_metadata_.video_rotation == VIDEO_ROTATION_270) {
      gfx::Size size = pipeline_metadata_.natural_size;
      pipeline_metadata_.natural_size = gfx::Size(size.height(), size.width());
    }

    video_weblayer_.reset(new cc_blink::WebLayerImpl(layer));
    video_weblayer_->layer()->SetContentsOpaque(opaque_);
    video_weblayer_->SetContentsOpaqueIsFixed(true);
    client_->setWebLayer(video_weblayer_.get());
  }

  // Tell the delegate we can now be safely suspended due to inactivity if a
  // subsequent play event does not occur.
  if (paused_)
    NotifyPlaybackPaused();

  // If the frame is hidden, it may be time to suspend playback.
  if (delegate_ && delegate_->IsHidden())
    OnHidden();
}

void WebMediaPlayerImpl::OnPipelineBufferingStateChanged(
    BufferingState buffering_state) {
  DVLOG(1) << __FUNCTION__ << "(" << buffering_state << ")";

  // Ignore buffering state changes until we've completed all outstanding
  // operations.
  if (!pipeline_controller_.IsStable())
    return;

  // TODO(scherkus): Handle other buffering states when Pipeline starts using
  // them and translate them ready state changes http://crbug.com/144683
  DCHECK_EQ(buffering_state, BUFFERING_HAVE_ENOUGH);
  SetReadyState(WebMediaPlayer::ReadyStateHaveEnoughData);

  // Let the DataSource know we have enough data. It may use this information to
  // release unused network connections.
  if (data_source_)
    data_source_->OnBufferingHaveEnough(false);

  // Blink expects a timeChanged() in response to a seek().
  if (should_notify_time_changed_)
    client_->timeChanged();

  // Once we have enough, start reporting the total memory usage. We'll also
  // report once playback starts.
  ReportMemoryUsage();
}

void WebMediaPlayerImpl::OnDemuxerOpened() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  client_->mediaSourceOpened(
      new WebMediaSourceImpl(chunk_demuxer_, media_log_));
}

void WebMediaPlayerImpl::OnAddTextTrack(
    const TextTrackConfig& config,
    const AddTextTrackDoneCB& done_cb) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  const WebInbandTextTrackImpl::Kind web_kind =
      static_cast<WebInbandTextTrackImpl::Kind>(config.kind());
  const blink::WebString web_label =
      blink::WebString::fromUTF8(config.label());
  const blink::WebString web_language =
      blink::WebString::fromUTF8(config.language());
  const blink::WebString web_id =
      blink::WebString::fromUTF8(config.id());

  scoped_ptr<WebInbandTextTrackImpl> web_inband_text_track(
      new WebInbandTextTrackImpl(web_kind, web_label, web_language, web_id));

  scoped_ptr<TextTrack> text_track(new TextTrackImpl(
      main_task_runner_, client_, std::move(web_inband_text_track)));

  done_cb.Run(std::move(text_track));
}

void WebMediaPlayerImpl::OnHidden() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  if (!IsSuspendUponHiddenEnabled())
    return;

#if defined(OS_ANDROID)  // WMPI_CAST
  // If we're remote, the pipeline should already be suspended.
  if (isRemote())
    return;
#endif

  // Don't suspend before metadata is available, as we don't know if there is a
  // video track yet.
  if (ready_state_ < WebMediaPlayer::ReadyStateHaveMetadata)
    return;

  // Don't suspend players which only have audio and have not completed
  // playback. The user can still control these players via the MediaSession UI.
  // If the player has never started playback, OnSuspendRequested() will handle
  // release of any idle resources.
  if (!hasVideo() && !paused_ && !ended_)
    return;

  // Always reset the buffering strategy to normal when suspending for hidden to
  // prevent an idle network connection from lingering.
  setBufferingStrategy(WebMediaPlayer::BufferingStrategy::Normal);
  pipeline_controller_.Suspend();
  // If we're in the middle of a suspend/resume cycle we no longer want to
  // resume when the suspend completes.
  pending_suspend_resume_cycle_ = false;
  if (delegate_)
    delegate_->PlayerGone(delegate_id_);
}

void WebMediaPlayerImpl::OnShown() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  if (!IsSuspendUponHiddenEnabled())
    return;

#if defined(OS_ANDROID)  // WMPI_CAST
  // If we're remote, the pipeline should stay suspended.
  if (isRemote())
    return;
#endif

  // If we do not yet have metadata, the only way we could have been suspended
  // is by a OnSuspendRequested() with |must_suspend| set. In that case we need
  // to resume, otherwise playback will be broken.
  //
  // Otherwise, resume if we should be playing.
  if (ready_state_ < WebMediaPlayer::ReadyStateHaveMetadata ||
      (!ended_ && !paused_)) {
    pipeline_controller_.Resume();
  }
}

void WebMediaPlayerImpl::OnSuspendRequested(bool must_suspend) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

#if defined(OS_ANDROID)  // WMPI_CAST
  // If we're remote, the pipeline should already be suspended.
  if (isRemote())
    return;
#endif

#if defined(OS_MACOSX)
  // TODO(sandersd): Idle suspend is disabled on OSX since hardware decoded
  // frames are owned by the video decoder in the GPU process. A mechanism for
  // detaching ownership from the decoder is needed. http://crbug.com/595716.
  return;
#else
  // Suspend should never be requested unless required or we're already in an
  // idle state (paused or ended).
  DCHECK(must_suspend || paused_ || ended_);

  // Always suspend, but only notify the delegate if we must; this allows any
  // exposed UI for player controls to continue to function even though the
  // player has now been suspended.
  pipeline_controller_.Suspend();
  if (must_suspend && delegate_)
    delegate_->PlayerGone(delegate_id_);
#endif
}

void WebMediaPlayerImpl::OnPlay() {
  play();
  client_->playbackStateChanged();
}

void WebMediaPlayerImpl::OnPause() {
  pause();
  client_->playbackStateChanged();
}

void WebMediaPlayerImpl::OnVolumeMultiplierUpdate(double multiplier) {
  volume_multiplier_ = multiplier;
  setVolume(volume_);
}

void WebMediaPlayerImpl::ScheduleRestart() {
  if (!pipeline_controller_.IsSuspended()) {
    pending_suspend_resume_cycle_ = true;
    pipeline_controller_.Suspend();
  }
}

#if defined(OS_ANDROID)  // WMPI_CAST
bool WebMediaPlayerImpl::isRemote() const {
  return cast_impl_.isRemote();
}

void WebMediaPlayerImpl::SetMediaPlayerManager(
    RendererMediaPlayerManagerInterface* media_player_manager) {
  cast_impl_.SetMediaPlayerManager(media_player_manager);
}

void WebMediaPlayerImpl::requestRemotePlayback() {
  cast_impl_.requestRemotePlayback();
}

void WebMediaPlayerImpl::requestRemotePlaybackControl() {
  cast_impl_.requestRemotePlaybackControl();
}

void WebMediaPlayerImpl::OnRemotePlaybackEnded() {
  DVLOG(1) << __FUNCTION__;
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  ended_ = true;
  client_->timeChanged();
}

void WebMediaPlayerImpl::OnDisconnectedFromRemoteDevice(double t) {
  DoSeek(base::TimeDelta::FromSecondsD(t), false);
  if (delegate_ && !delegate_->IsHidden())
    pipeline_controller_.Resume();

  // We already told the delegate we're paused when remoting started.
  client_->playbackStateChanged();
  client_->disconnectedFromRemoteDevice();
}

void WebMediaPlayerImpl::SuspendForRemote() {
  if (!pipeline_controller_.IsSuspended()) {
    pipeline_controller_.Suspend();
  } else {
    // TODO(sandersd): If PipelineController::Suspend() called |suspended_cb|
    // when already suspended, we wouldn't need this case.
    scoped_refptr<VideoFrame> frame = cast_impl_.GetCastingBanner();
    if (frame) {
      compositor_->PaintFrameUsingOldRenderingPath(frame);
    }
  }
}

gfx::Size WebMediaPlayerImpl::GetCanvasSize() const {
  if (!video_weblayer_)
    return pipeline_metadata_.natural_size;

  return video_weblayer_->bounds();
}

void WebMediaPlayerImpl::SetDeviceScaleFactor(float scale_factor) {
  cast_impl_.SetDeviceScaleFactor(scale_factor);
}
#endif  // defined(OS_ANDROID)  // WMPI_CAST

void WebMediaPlayerImpl::DataSourceInitialized(bool success) {
  DVLOG(1) << __FUNCTION__;
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  if (!success) {
    SetNetworkState(WebMediaPlayer::NetworkStateFormatError);
    return;
  }

  StartPipeline();
}

void WebMediaPlayerImpl::NotifyDownloading(bool is_downloading) {
  DVLOG(1) << __FUNCTION__;
  if (!is_downloading && network_state_ == WebMediaPlayer::NetworkStateLoading)
    SetNetworkState(WebMediaPlayer::NetworkStateIdle);
  else if (is_downloading && network_state_ == WebMediaPlayer::NetworkStateIdle)
    SetNetworkState(WebMediaPlayer::NetworkStateLoading);
  media_log_->AddEvent(
      media_log_->CreateBooleanEvent(
          MediaLogEvent::NETWORK_ACTIVITY_SET,
          "is_downloading_data", is_downloading));
}

// TODO(watk): Move this state management out of WMPI.
void WebMediaPlayerImpl::OnSurfaceRequested(
    const SurfaceCreatedCB& surface_created_cb) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  DCHECK(surface_manager_);

  // A null callback indicates that the decoder is going away.
  if (surface_created_cb.is_null()) {
    decoder_requires_restart_for_fullscreen_ = false;
    return;
  }

  // If we're getting a surface request it means GVD is initializing, so until
  // we get a null surface request, GVD is the active decoder. While that's the
  // case we should restart the pipeline on fullscreen transitions so that when
  // we create a new GVD it will request a surface again and get the right kind
  // of surface for the fullscreen state.
  // TODO(watk): Don't require a pipeline restart to switch surfaces for
  // cases where it isn't necessary.
  decoder_requires_restart_for_fullscreen_ = true;
  if (fullscreen_) {
    surface_manager_->CreateFullscreenSurface(pipeline_metadata_.natural_size,
                                              surface_created_cb);
  } else {
    // Tell the decoder to create its own surface.
    surface_created_cb.Run(SurfaceManager::kNoSurfaceID);
  }
}

scoped_ptr<Renderer> WebMediaPlayerImpl::CreateRenderer() {
  RequestSurfaceCB request_surface_cb;
#if defined(OS_ANDROID)
  request_surface_cb =
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnSurfaceRequested);
#endif
  return renderer_factory_->CreateRenderer(
      media_task_runner_, worker_task_runner_, audio_source_provider_.get(),
      compositor_, request_surface_cb);
}

void WebMediaPlayerImpl::StartPipeline() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  Demuxer::EncryptedMediaInitDataCB encrypted_media_init_data_cb =
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnEncryptedMediaInitData);

  // Figure out which demuxer to use.
  if (load_type_ != LoadTypeMediaSource) {
    DCHECK(!chunk_demuxer_);
    DCHECK(data_source_);

#if !defined(MEDIA_DISABLE_FFMPEG)
    Demuxer::MediaTracksUpdatedCB media_tracks_updated_cb =
        base::Bind(&WebMediaPlayerImpl::OnFFmpegMediaTracksUpdated,
                   base::Unretained(this));

    demuxer_.reset(new FFmpegDemuxer(media_task_runner_, data_source_.get(),
                                     encrypted_media_init_data_cb,
                                     media_tracks_updated_cb, media_log_));
#else
    OnPipelineError(PipelineStatus::DEMUXER_ERROR_COULD_NOT_OPEN);
    return;
#endif
  } else {
    DCHECK(!chunk_demuxer_);
    DCHECK(!data_source_);

    chunk_demuxer_ = new ChunkDemuxer(
        BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnDemuxerOpened),
        encrypted_media_init_data_cb, media_log_, true);
    demuxer_.reset(chunk_demuxer_);
  }

  // TODO(sandersd): FileSystem objects may also be non-static, but due to our
  // caching layer such situations are broken already. http://crbug.com/593159
  bool is_static = !chunk_demuxer_;

  // ... and we're ready to go!
  seeking_ = true;

  // TODO(sandersd): On Android, defer Start() if the tab is not visible.
  bool is_streaming = (data_source_ && data_source_->IsStreaming());
  pipeline_controller_.Start(
      demuxer_.get(), is_streaming, is_static,
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnPipelineEnded),
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnPipelineMetadata),
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnPipelineBufferingStateChanged),
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnDurationChanged),
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnAddTextTrack),
      BIND_TO_RENDER_LOOP(&WebMediaPlayerImpl::OnWaitingForDecryptionKey));
}

void WebMediaPlayerImpl::SetNetworkState(WebMediaPlayer::NetworkState state) {
  DVLOG(1) << __FUNCTION__ << "(" << state << ")";
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  network_state_ = state;
  // Always notify to ensure client has the latest value.
  client_->networkStateChanged();
}

void WebMediaPlayerImpl::SetReadyState(WebMediaPlayer::ReadyState state) {
  DVLOG(1) << __FUNCTION__ << "(" << state << ")";
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  if (state == WebMediaPlayer::ReadyStateHaveEnoughData && data_source_ &&
      data_source_->assume_fully_buffered() &&
      network_state_ == WebMediaPlayer::NetworkStateLoading)
    SetNetworkState(WebMediaPlayer::NetworkStateLoaded);

  ready_state_ = state;
  // Always notify to ensure client has the latest value.
  client_->readyStateChanged();
}

blink::WebAudioSourceProvider* WebMediaPlayerImpl::getAudioSourceProvider() {
  return audio_source_provider_.get();
}

double WebMediaPlayerImpl::GetPipelineDuration() const {
  base::TimeDelta duration = pipeline_.GetMediaDuration();

  // Return positive infinity if the resource is unbounded.
  // http://www.whatwg.org/specs/web-apps/current-work/multipage/video.html#dom-media-duration
  if (duration == kInfiniteDuration())
    return std::numeric_limits<double>::infinity();

  return duration.InSecondsF();
}

void WebMediaPlayerImpl::OnDurationChanged() {
  if (ready_state_ == WebMediaPlayer::ReadyStateHaveNothing)
    return;

  client_->durationChanged();
}

void WebMediaPlayerImpl::OnNaturalSizeChanged(gfx::Size size) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  DCHECK_NE(ready_state_, WebMediaPlayer::ReadyStateHaveNothing);
  TRACE_EVENT0("media", "WebMediaPlayerImpl::OnNaturalSizeChanged");

  media_log_->AddEvent(
      media_log_->CreateVideoSizeSetEvent(size.width(), size.height()));

  if (fullscreen_ && surface_manager_ &&
      pipeline_metadata_.natural_size != size) {
    surface_manager_->NaturalSizeChanged(size);
  }

  pipeline_metadata_.natural_size = size;
  client_->sizeChanged();
}

void WebMediaPlayerImpl::OnOpacityChanged(bool opaque) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());
  DCHECK_NE(ready_state_, WebMediaPlayer::ReadyStateHaveNothing);

  opaque_ = opaque;
  // Modify content opaqueness of cc::Layer directly so that
  // SetContentsOpaqueIsFixed is ignored.
  if (video_weblayer_)
    video_weblayer_->layer()->SetContentsOpaque(opaque_);
}

static void GetCurrentFrameAndSignal(
    VideoFrameCompositor* compositor,
    scoped_refptr<VideoFrame>* video_frame_out,
    base::WaitableEvent* event) {
  TRACE_EVENT0("media", "GetCurrentFrameAndSignal");
  *video_frame_out = compositor->GetCurrentFrameAndUpdateIfStale();
  event->Signal();
}

scoped_refptr<VideoFrame>
WebMediaPlayerImpl::GetCurrentFrameFromCompositor() {
  TRACE_EVENT0("media", "WebMediaPlayerImpl::GetCurrentFrameFromCompositor");
  if (compositor_task_runner_->BelongsToCurrentThread())
    return compositor_->GetCurrentFrameAndUpdateIfStale();

  // Use a posted task and waitable event instead of a lock otherwise
  // WebGL/Canvas can see different content than what the compositor is seeing.
  scoped_refptr<VideoFrame> video_frame;
  base::WaitableEvent event(false, false);
  compositor_task_runner_->PostTask(FROM_HERE,
                                    base::Bind(&GetCurrentFrameAndSignal,
                                               base::Unretained(compositor_),
                                               &video_frame,
                                               &event));
  event.Wait();
  return video_frame;
}

void WebMediaPlayerImpl::NotifyPlaybackStarted() {
#if defined(OS_ANDROID)  // WMPI_CAST
  // We do not tell our delegates about remote playback, because that would
  // keep the device awake, which is not what we want.
  if (isRemote())
    return;
#endif

  // NotifyPlaybackStarted() may be called by interactions while suspended,
  // (play/pause in particular). Those actions won't have any effect until the
  // pipeline is resumed.
  // TODO(dalecurtis): Should these be dropped at the call sites instead?
  // Alternatively, rename this method to include Maybe or Changed, and handle
  // multiple calls safely.
  if (pipeline_controller_.IsSuspended())
    return;

  if (delegate_) {
    delegate_->DidPlay(delegate_id_, hasVideo(), hasAudio(), false,
                       pipeline_.GetMediaDuration());
  }
  if (!memory_usage_reporting_timer_.IsRunning()) {
    memory_usage_reporting_timer_.Start(FROM_HERE,
                                        base::TimeDelta::FromSeconds(2), this,
                                        &WebMediaPlayerImpl::ReportMemoryUsage);
  }
}

void WebMediaPlayerImpl::NotifyPlaybackPaused() {
#if defined(OS_ANDROID)  // WMPI_CAST
  if (isRemote())
    return;
#endif

  // Same as above, NotifyPlaybackPaused() may be called by interactions while
  // suspended, but those actions won't have any effect until the pipeline is
  // resumed.
  if (pipeline_controller_.IsSuspended())
    return;

  if (delegate_)
    delegate_->DidPause(delegate_id_, ended_);
  memory_usage_reporting_timer_.Stop();
  ReportMemoryUsage();
}

void WebMediaPlayerImpl::ReportMemoryUsage() {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  // About base::Unretained() usage below: We destroy |demuxer_| on the main
  // thread.  Before that, however, ~WebMediaPlayerImpl() posts a task to the
  // media thread and waits for it to finish.  Hence, the GetMemoryUsage() task
  // posted here must finish earlier.

  if (demuxer_) {
    base::PostTaskAndReplyWithResult(
        media_task_runner_.get(), FROM_HERE,
        base::Bind(&Demuxer::GetMemoryUsage, base::Unretained(demuxer_.get())),
        base::Bind(&WebMediaPlayerImpl::FinishMemoryUsageReport, AsWeakPtr()));
  } else {
    FinishMemoryUsageReport(0);
  }
}

void WebMediaPlayerImpl::FinishMemoryUsageReport(int64_t demuxer_memory_usage) {
  DCHECK(main_task_runner_->BelongsToCurrentThread());

  const PipelineStatistics stats = pipeline_.GetStatistics();
  const int64_t current_memory_usage =
      stats.audio_memory_usage + stats.video_memory_usage +
      (data_source_ ? data_source_->GetMemoryUsage() : 0) +
      demuxer_memory_usage;

  // Note, this isn't entirely accurate, there may be VideoFrames held by the
  // compositor or other resources that we're unaware of.

  DVLOG(2) << "Memory Usage -- Audio: " << stats.audio_memory_usage
           << ", Video: " << stats.video_memory_usage << ", DataSource: "
           << (data_source_ ? data_source_->GetMemoryUsage() : 0)
           << ", Demuxer: " << demuxer_memory_usage;

  const int64_t delta = current_memory_usage - last_reported_memory_usage_;
  last_reported_memory_usage_ = current_memory_usage;
  adjust_allocated_memory_cb_.Run(delta);
}

bool WebMediaPlayerImpl::IsAutomaticResumeAllowed() {
#if defined(OS_ANDROID)
  return !hasVideo() || (delegate_ && !delegate_->IsHidden());
#else
  // On non-Android platforms Resume() is always allowed.
  return true;
#endif
}

}  // namespace media
