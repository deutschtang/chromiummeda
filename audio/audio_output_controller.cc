// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/audio_output_controller.h"

#include "base/bind.h"
#include "base/debug/trace_event.h"
#include "base/message_loop/message_loop.h"
#include "base/metrics/histogram.h"
#include "base/task_runner_util.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "media/base/scoped_histogram_timer.h"

using base::Time;
using base::TimeDelta;

namespace media {

#if defined(AUDIO_POWER_MONITORING)
// Time constant for AudioPowerMonitor.  See AudioPowerMonitor ctor comments for
// semantics.  This value was arbitrarily chosen, but seems to work well.
static const int kPowerMeasurementTimeConstantMillis = 10;

// Desired frequency of calls to EventHandler::OnPowerMeasured() for reporting
// power levels in the audio signal.
static const int kPowerMeasurementsPerSecond = 4;
#endif

// Polling-related constants.
const int AudioOutputController::kPollNumAttempts = 3;
const int AudioOutputController::kPollPauseInMilliseconds = 3;

AudioOutputController::AudioOutputController(
    AudioManager* audio_manager,
    EventHandler* handler,
    const AudioParameters& params,
    const std::string& output_device_id,
    const std::string& input_device_id,
    SyncReader* sync_reader)
    : audio_manager_(audio_manager),
      params_(params),
      handler_(handler),
      output_device_id_(output_device_id),
      input_device_id_(input_device_id),
      stream_(NULL),
      diverting_to_stream_(NULL),
      volume_(1.0),
      state_(kEmpty),
      num_allowed_io_(0),
      sync_reader_(sync_reader),
      message_loop_(audio_manager->GetMessageLoop()),
#if defined(AUDIO_POWER_MONITORING)
      power_monitor_(
          params.sample_rate(),
          TimeDelta::FromMilliseconds(kPowerMeasurementTimeConstantMillis)),
#endif
      on_more_io_data_called_(0) {
  DCHECK(audio_manager);
  DCHECK(handler_);
  DCHECK(sync_reader_);
  DCHECK(message_loop_.get());
}

AudioOutputController::~AudioOutputController() {
  DCHECK_EQ(kClosed, state_);
}

// static
scoped_refptr<AudioOutputController> AudioOutputController::Create(
    AudioManager* audio_manager,
    EventHandler* event_handler,
    const AudioParameters& params,
    const std::string& output_device_id,
    const std::string& input_device_id,
    SyncReader* sync_reader) {
  DCHECK(audio_manager);
  DCHECK(sync_reader);

  if (!params.IsValid() || !audio_manager)
    return NULL;

  scoped_refptr<AudioOutputController> controller(new AudioOutputController(
      audio_manager, event_handler, params, output_device_id, input_device_id,
      sync_reader));
  controller->message_loop_->PostTask(FROM_HERE, base::Bind(
      &AudioOutputController::DoCreate, controller, false));
  return controller;
}

void AudioOutputController::Play() {
  message_loop_->PostTask(FROM_HERE, base::Bind(
      &AudioOutputController::DoPlay, this));
}

void AudioOutputController::Pause() {
  message_loop_->PostTask(FROM_HERE, base::Bind(
      &AudioOutputController::DoPause, this));
}

void AudioOutputController::Close(const base::Closure& closed_task) {
  DCHECK(!closed_task.is_null());
  message_loop_->PostTaskAndReply(FROM_HERE, base::Bind(
      &AudioOutputController::DoClose, this), closed_task);
}

void AudioOutputController::SetVolume(double volume) {
  message_loop_->PostTask(FROM_HERE, base::Bind(
      &AudioOutputController::DoSetVolume, this, volume));
}

void AudioOutputController::GetOutputDeviceId(
    base::Callback<void(const std::string&)> callback) const {
  base::PostTaskAndReplyWithResult(
      message_loop_.get(),
      FROM_HERE,
      base::Bind(&AudioOutputController::DoGetOutputDeviceId, this),
      callback);
}

void AudioOutputController::SwitchOutputDevice(
    const std::string& output_device_id, const base::Closure& callback) {
  message_loop_->PostTaskAndReply(
      FROM_HERE,
      base::Bind(&AudioOutputController::DoSwitchOutputDevice, this,
                 output_device_id),
      callback);
}

void AudioOutputController::DoCreate(bool is_for_device_change) {
  DCHECK(message_loop_->BelongsToCurrentThread());
  SCOPED_UMA_HISTOGRAM_TIMER("Media.AudioOutputController.CreateTime");
  TRACE_EVENT0("audio", "AudioOutputController::DoCreate");

  // Close() can be called before DoCreate() is executed.
  if (state_ == kClosed)
    return;

  DoStopCloseAndClearStream();  // Calls RemoveOutputDeviceChangeListener().
  DCHECK_EQ(kEmpty, state_);

  stream_ = diverting_to_stream_ ?
      diverting_to_stream_ :
      audio_manager_->MakeAudioOutputStreamProxy(params_, output_device_id_,
                                                 input_device_id_);
  if (!stream_) {
    state_ = kError;
    handler_->OnError();
    return;
  }

  if (!stream_->Open()) {
    DoStopCloseAndClearStream();
    state_ = kError;
    handler_->OnError();
    return;
  }

  // Everything started okay, so re-register for state change callbacks if
  // stream_ was created via AudioManager.
  if (stream_ != diverting_to_stream_)
    audio_manager_->AddOutputDeviceChangeListener(this);

  // We have successfully opened the stream. Set the initial volume.
  stream_->SetVolume(volume_);

  // Finally set the state to kCreated.
  state_ = kCreated;

  // And then report we have been created if we haven't done so already.
  if (!is_for_device_change)
    handler_->OnCreated();
}

void AudioOutputController::DoPlay() {
  DCHECK(message_loop_->BelongsToCurrentThread());
  SCOPED_UMA_HISTOGRAM_TIMER("Media.AudioOutputController.PlayTime");
  TRACE_EVENT0("audio", "AudioOutputController::DoPlay");

  // We can start from created or paused state.
  if (state_ != kCreated && state_ != kPaused)
    return;

  // Ask for first packet.
  sync_reader_->UpdatePendingBytes(0);

  state_ = kPlaying;

#if defined(AUDIO_POWER_MONITORING)
  power_monitor_.Reset();
  power_poll_callback_.Reset(
      base::Bind(&AudioOutputController::ReportPowerMeasurementPeriodically,
                 this));
  // Run the callback to send an initial notification that we're starting in
  // silence, and to schedule periodic callbacks.
  power_poll_callback_.callback().Run();
#endif

  on_more_io_data_called_ = 0;
  AllowEntryToOnMoreIOData();
  stream_->Start(this);

  // For UMA tracking purposes, start the wedge detection timer.  This allows us
  // to record statistics about the number of wedged playbacks in the field.
  //
  // WedgeCheck() will look to see if |on_more_io_data_called_| is true after
  // the timeout expires.  Care must be taken to ensure the wedge check delay is
  // large enough that the value isn't queried while OnMoreDataIO() is setting
  // it.
  //
  // Timer self-manages its lifetime and WedgeCheck() will only record the UMA
  // statistic if state is still kPlaying.  Additional Start() calls will
  // invalidate the previous timer.
  wedge_timer_.reset(new base::OneShotTimer<AudioOutputController>());
  wedge_timer_->Start(
      FROM_HERE, TimeDelta::FromSeconds(5), this,
      &AudioOutputController::WedgeCheck);

  handler_->OnPlaying();
}

#if defined(AUDIO_POWER_MONITORING)
void AudioOutputController::ReportPowerMeasurementPeriodically() {
  DCHECK(message_loop_->BelongsToCurrentThread());
  const std::pair<float, bool>& reading =
      power_monitor_.ReadCurrentPowerAndClip();
  handler_->OnPowerMeasured(reading.first, reading.second);
  message_loop_->PostDelayedTask(
      FROM_HERE, power_poll_callback_.callback(),
      TimeDelta::FromSeconds(1) / kPowerMeasurementsPerSecond);
}
#endif

void AudioOutputController::StopStream() {
  DCHECK(message_loop_->BelongsToCurrentThread());

  if (state_ == kPlaying) {
    wedge_timer_.reset();
    stream_->Stop();
    DisallowEntryToOnMoreIOData();

#if defined(AUDIO_POWER_MONITORING)
    power_poll_callback_.Cancel();
#endif

    state_ = kPaused;
  }
}

void AudioOutputController::DoPause() {
  DCHECK(message_loop_->BelongsToCurrentThread());
  SCOPED_UMA_HISTOGRAM_TIMER("Media.AudioOutputController.PauseTime");
  TRACE_EVENT0("audio", "AudioOutputController::DoPause");

  StopStream();

  if (state_ != kPaused)
    return;

  // Let the renderer know we've stopped.  Necessary to let PPAPI clients know
  // audio has been shutdown.  TODO(dalecurtis): This stinks.  PPAPI should have
  // a better way to know when it should exit PPB_Audio_Shared::Run().
  sync_reader_->UpdatePendingBytes(-1);

#if defined(AUDIO_POWER_MONITORING)
  // Paused means silence follows.
  handler_->OnPowerMeasured(AudioPowerMonitor::zero_power(), false);
#endif

  handler_->OnPaused();
}

void AudioOutputController::DoClose() {
  DCHECK(message_loop_->BelongsToCurrentThread());
  SCOPED_UMA_HISTOGRAM_TIMER("Media.AudioOutputController.CloseTime");
  TRACE_EVENT0("audio", "AudioOutputController::DoClose");

  if (state_ != kClosed) {
    DoStopCloseAndClearStream();
    sync_reader_->Close();
    state_ = kClosed;
  }
}

void AudioOutputController::DoSetVolume(double volume) {
  DCHECK(message_loop_->BelongsToCurrentThread());

  // Saves the volume to a member first. We may not be able to set the volume
  // right away but when the stream is created we'll set the volume.
  volume_ = volume;

  switch (state_) {
    case kCreated:
    case kPlaying:
    case kPaused:
      stream_->SetVolume(volume_);
      break;
    default:
      return;
  }
}

std::string AudioOutputController::DoGetOutputDeviceId() const {
  DCHECK(message_loop_->BelongsToCurrentThread());
  return output_device_id_;
}

void AudioOutputController::DoSwitchOutputDevice(
    const std::string& output_device_id) {
  DCHECK(message_loop_->BelongsToCurrentThread());

  if (state_ == kClosed)
    return;

  output_device_id_ = output_device_id;

  // If output is currently diverted, we must not call OnDeviceChange
  // since it would break the diverted setup. Once diversion is
  // finished using StopDiverting() the output will switch to the new
  // device ID.
  if (stream_ != diverting_to_stream_)
    OnDeviceChange();
}

void AudioOutputController::DoReportError() {
  DCHECK(message_loop_->BelongsToCurrentThread());
  if (state_ != kClosed)
    handler_->OnError();
}

int AudioOutputController::OnMoreData(AudioBus* dest,
                                      AudioBuffersState buffers_state) {
  return OnMoreIOData(NULL, dest, buffers_state);
}

int AudioOutputController::OnMoreIOData(AudioBus* source,
                                        AudioBus* dest,
                                        AudioBuffersState buffers_state) {
  DisallowEntryToOnMoreIOData();
  TRACE_EVENT0("audio", "AudioOutputController::OnMoreIOData");

  // Indicate that we haven't wedged (at least not indefinitely, WedgeCheck()
  // may have already fired if OnMoreIOData() took an abnormal amount of time).
  // Since this thread is the only writer of |on_more_io_data_called_| once the
  // thread starts, its safe to compare and then increment.
  if (base::AtomicRefCountIsZero(&on_more_io_data_called_))
    base::AtomicRefCountInc(&on_more_io_data_called_);

  sync_reader_->Read(source, dest);

  const int frames = dest->frames();
  sync_reader_->UpdatePendingBytes(
      buffers_state.total_bytes() + frames * params_.GetBytesPerFrame());

#if defined(AUDIO_POWER_MONITORING)
  power_monitor_.Scan(*dest, frames);
#endif

  AllowEntryToOnMoreIOData();
  return frames;
}

void AudioOutputController::OnError(AudioOutputStream* stream) {
  // Handle error on the audio controller thread.
  message_loop_->PostTask(FROM_HERE, base::Bind(
      &AudioOutputController::DoReportError, this));
}

void AudioOutputController::DoStopCloseAndClearStream() {
  DCHECK(message_loop_->BelongsToCurrentThread());

  // Allow calling unconditionally and bail if we don't have a stream_ to close.
  if (stream_) {
    // De-register from state change callbacks if stream_ was created via
    // AudioManager.
    if (stream_ != diverting_to_stream_)
      audio_manager_->RemoveOutputDeviceChangeListener(this);

    StopStream();
    stream_->Close();
    if (stream_ == diverting_to_stream_)
      diverting_to_stream_ = NULL;
    stream_ = NULL;
  }

  state_ = kEmpty;
}

void AudioOutputController::OnDeviceChange() {
  DCHECK(message_loop_->BelongsToCurrentThread());
  SCOPED_UMA_HISTOGRAM_TIMER("Media.AudioOutputController.DeviceChangeTime");
  TRACE_EVENT0("audio", "AudioOutputController::OnDeviceChange");

  // TODO(dalecurtis): Notify the renderer side that a device change has
  // occurred.  Currently querying the hardware information here will lead to
  // crashes on OSX.  See http://crbug.com/158170.

  // Recreate the stream (DoCreate() will first shut down an existing stream).
  // Exit if we ran into an error.
  const State original_state = state_;
  DoCreate(true);
  if (!stream_ || state_ == kError)
    return;

  // Get us back to the original state or an equivalent state.
  switch (original_state) {
    case kPlaying:
      DoPlay();
      return;
    case kCreated:
    case kPaused:
      // From the outside these two states are equivalent.
      return;
    default:
      NOTREACHED() << "Invalid original state.";
  }
}

const AudioParameters& AudioOutputController::GetAudioParameters() {
  return params_;
}

void AudioOutputController::StartDiverting(AudioOutputStream* to_stream) {
  message_loop_->PostTask(
      FROM_HERE,
      base::Bind(&AudioOutputController::DoStartDiverting, this, to_stream));
}

void AudioOutputController::StopDiverting() {
  message_loop_->PostTask(
      FROM_HERE, base::Bind(&AudioOutputController::DoStopDiverting, this));
}

void AudioOutputController::DoStartDiverting(AudioOutputStream* to_stream) {
  DCHECK(message_loop_->BelongsToCurrentThread());

  if (state_ == kClosed)
    return;

  DCHECK(!diverting_to_stream_);
  diverting_to_stream_ = to_stream;
  // Note: OnDeviceChange() will engage the "re-create" process, which will
  // detect and use the alternate AudioOutputStream rather than create a new one
  // via AudioManager.
  OnDeviceChange();
}

void AudioOutputController::DoStopDiverting() {
  DCHECK(message_loop_->BelongsToCurrentThread());

  if (state_ == kClosed)
    return;

  // Note: OnDeviceChange() will cause the existing stream (the consumer of the
  // diverted audio data) to be closed, and diverting_to_stream_ will be set
  // back to NULL.
  OnDeviceChange();
  DCHECK(!diverting_to_stream_);
}

void AudioOutputController::AllowEntryToOnMoreIOData() {
  DCHECK(base::AtomicRefCountIsZero(&num_allowed_io_));
  base::AtomicRefCountInc(&num_allowed_io_);
}

void AudioOutputController::DisallowEntryToOnMoreIOData() {
  const bool is_zero = !base::AtomicRefCountDec(&num_allowed_io_);
  DCHECK(is_zero);
}

void AudioOutputController::WedgeCheck() {
  DCHECK(message_loop_->BelongsToCurrentThread());

  // If we should be playing and we haven't, that's a wedge.
  if (state_ == kPlaying) {
    UMA_HISTOGRAM_BOOLEAN("Media.AudioOutputControllerPlaybackStartupSuccess",
                          base::AtomicRefCountIsOne(&on_more_io_data_called_));

    // TODO(dalecurtis): Temporary debugging measure, see crbug.com/316376
    CHECK(base::AtomicRefCountIsOne(&on_more_io_data_called_));
  }
}

}  // namespace media
