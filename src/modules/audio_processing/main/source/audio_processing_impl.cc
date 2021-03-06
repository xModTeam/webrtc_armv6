/*
 *  Copyright (c) 2011 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "audio_processing_impl.h"

#include <cassert>

#include "module_common_types.h"

#include "critical_section_wrapper.h"
#include "file_wrapper.h"

#include "audio_buffer.h"
#include "echo_cancellation_impl.h"
#include "echo_control_mobile_impl.h"
#include "high_pass_filter_impl.h"
#include "gain_control_impl.h"
#include "level_estimator_impl.h"
#include "noise_suppression_impl.h"
#include "processing_component.h"
#include "splitting_filter.h"
#include "voice_detection_impl.h"

namespace webrtc {
namespace {

enum Events {
  kInitializeEvent,
  kRenderEvent,
  kCaptureEvent
};

const char kMagicNumber[] = "#!vqetrace1.2";
}  // namespace

AudioProcessing* AudioProcessing::Create(int id) {
  /*WEBRTC_TRACE(webrtc::kTraceModuleCall,
             webrtc::kTraceAudioProcessing,
             id,
             "AudioProcessing::Create()");*/

  AudioProcessingImpl* apm = new AudioProcessingImpl(id);
  if (apm->Initialize() != kNoError) {
    delete apm;
    apm = NULL;
  }

  return apm;
}

void AudioProcessing::Destroy(AudioProcessing* apm) {
  delete static_cast<AudioProcessingImpl*>(apm);
}

AudioProcessingImpl::AudioProcessingImpl(int id)
    : id_(id),
      echo_cancellation_(NULL),
      echo_control_mobile_(NULL),
      gain_control_(NULL),
      high_pass_filter_(NULL),
      level_estimator_(NULL),
      noise_suppression_(NULL),
      voice_detection_(NULL),
      debug_file_(FileWrapper::Create()),
      crit_(CriticalSectionWrapper::CreateCriticalSection()),
      render_audio_(NULL),
      capture_audio_(NULL),
      sample_rate_hz_(kSampleRate16kHz),
      split_sample_rate_hz_(kSampleRate16kHz),
      samples_per_channel_(sample_rate_hz_ / 100),
      stream_delay_ms_(0),
      was_stream_delay_set_(false),
      num_render_input_channels_(1),
      num_capture_input_channels_(1),
      num_capture_output_channels_(1) {

  echo_cancellation_ = new EchoCancellationImpl(this);
  component_list_.push_back(echo_cancellation_);

  echo_control_mobile_ = new EchoControlMobileImpl(this);
  component_list_.push_back(echo_control_mobile_);

  gain_control_ = new GainControlImpl(this);
  component_list_.push_back(gain_control_);

  high_pass_filter_ = new HighPassFilterImpl(this);
  component_list_.push_back(high_pass_filter_);

  level_estimator_ = new LevelEstimatorImpl(this);
  component_list_.push_back(level_estimator_);

  noise_suppression_ = new NoiseSuppressionImpl(this);
  component_list_.push_back(noise_suppression_);

  voice_detection_ = new VoiceDetectionImpl(this);
  component_list_.push_back(voice_detection_);
}

AudioProcessingImpl::~AudioProcessingImpl() {
  while (!component_list_.empty()) {
    ProcessingComponent* component = component_list_.front();
    component->Destroy();
    delete component;
    component_list_.pop_front();
  }

  if (debug_file_->Open()) {
    debug_file_->CloseFile();
  }
  delete debug_file_;
  debug_file_ = NULL;

  delete crit_;
  crit_ = NULL;

  if (render_audio_ != NULL) {
    delete render_audio_;
    render_audio_ = NULL;
  }

  if (capture_audio_ != NULL) {
    delete capture_audio_;
    capture_audio_ = NULL;
  }
}

CriticalSectionWrapper* AudioProcessingImpl::crit() const {
  return crit_;
}

int AudioProcessingImpl::split_sample_rate_hz() const {
  return split_sample_rate_hz_;
}

int AudioProcessingImpl::Initialize() {
  CriticalSectionScoped crit_scoped(*crit_);
  return InitializeLocked();
}

int AudioProcessingImpl::InitializeLocked() {
  if (render_audio_ != NULL) {
    delete render_audio_;
    render_audio_ = NULL;
  }

  if (capture_audio_ != NULL) {
    delete capture_audio_;
    capture_audio_ = NULL;
  }

  render_audio_ = new AudioBuffer(num_render_input_channels_,
                                  samples_per_channel_);
  capture_audio_ = new AudioBuffer(num_capture_input_channels_,
                                   samples_per_channel_);

  was_stream_delay_set_ = false;

  // Initialize all components.
  std::list<ProcessingComponent*>::iterator it;
  for (it = component_list_.begin(); it != component_list_.end(); it++) {
    int err = (*it)->Initialize();
    if (err != kNoError) {
      return err;
    }
  }

  return kNoError;
}

int AudioProcessingImpl::set_sample_rate_hz(int rate) {
  CriticalSectionScoped crit_scoped(*crit_);
  if (rate != kSampleRate8kHz &&
      rate != kSampleRate16kHz &&
      rate != kSampleRate32kHz) {
    return kBadParameterError;
  }

  sample_rate_hz_ = rate;
  samples_per_channel_ = rate / 100;

  if (sample_rate_hz_ == kSampleRate32kHz) {
    split_sample_rate_hz_ = kSampleRate16kHz;
  } else {
    split_sample_rate_hz_ = sample_rate_hz_;
  }

  return InitializeLocked();
}

int AudioProcessingImpl::sample_rate_hz() const {
  return sample_rate_hz_;
}

int AudioProcessingImpl::set_num_reverse_channels(int channels) {
  CriticalSectionScoped crit_scoped(*crit_);
  // Only stereo supported currently.
  if (channels > 2 || channels < 1) {
    return kBadParameterError;
  }

  num_render_input_channels_ = channels;

  return InitializeLocked();
}

int AudioProcessingImpl::num_reverse_channels() const {
  return num_render_input_channels_;
}

int AudioProcessingImpl::set_num_channels(
    int input_channels,
    int output_channels) {
  CriticalSectionScoped crit_scoped(*crit_);
  if (output_channels > input_channels) {
    return kBadParameterError;
  }

  // Only stereo supported currently.
  if (input_channels > 2 || input_channels < 1) {
    return kBadParameterError;
  }

  if (output_channels > 2 || output_channels < 1) {
    return kBadParameterError;
  }

  num_capture_input_channels_ = input_channels;
  num_capture_output_channels_ = output_channels;

  return InitializeLocked();
}

int AudioProcessingImpl::num_input_channels() const {
  return num_capture_input_channels_;
}

int AudioProcessingImpl::num_output_channels() const {
  return num_capture_output_channels_;
}

int AudioProcessingImpl::ProcessStream(AudioFrame* frame) {
  CriticalSectionScoped crit_scoped(*crit_);
  int err = kNoError;

  if (frame == NULL) {
    return kNullPointerError;
  }

  if (frame->_frequencyInHz !=
      static_cast<WebRtc_UWord32>(sample_rate_hz_)) {
    return kBadSampleRateError;
  }

  if (frame->_audioChannel != num_capture_input_channels_) {
    return kBadNumberChannelsError;
  }

  if (frame->_payloadDataLengthInSamples != samples_per_channel_) {
    return kBadDataLengthError;
  }

  if (debug_file_->Open()) {
    WebRtc_UWord8 event = kCaptureEvent;
    if (!debug_file_->Write(&event, sizeof(event))) {
      return kFileError;
    }

    if (!debug_file_->Write(&frame->_frequencyInHz,
                                   sizeof(frame->_frequencyInHz))) {
      return kFileError;
    }

    if (!debug_file_->Write(&frame->_audioChannel,
                                   sizeof(frame->_audioChannel))) {
      return kFileError;
    }

    if (!debug_file_->Write(&frame->_payloadDataLengthInSamples,
        sizeof(frame->_payloadDataLengthInSamples))) {
      return kFileError;
    }

    if (!debug_file_->Write(frame->_payloadData,
        sizeof(WebRtc_Word16) * frame->_payloadDataLengthInSamples *
        frame->_audioChannel)) {
      return kFileError;
    }
  }

  capture_audio_->DeinterleaveFrom(frame);

  // TODO(ajm): experiment with mixing and AEC placement.
  if (num_capture_output_channels_ < num_capture_input_channels_) {
    capture_audio_->Mix(num_capture_output_channels_);

    frame->_audioChannel = num_capture_output_channels_;
  }

  if (sample_rate_hz_ == kSampleRate32kHz) {
    for (int i = 0; i < num_capture_input_channels_; i++) {
      // Split into a low and high band.
      SplittingFilterAnalysis(capture_audio_->data(i),
                              capture_audio_->low_pass_split_data(i),
                              capture_audio_->high_pass_split_data(i),
                              capture_audio_->analysis_filter_state1(i),
                              capture_audio_->analysis_filter_state2(i));
    }
  }

  err = high_pass_filter_->ProcessCaptureAudio(capture_audio_);
  if (err != kNoError) {
    return err;
  }

  err = gain_control_->AnalyzeCaptureAudio(capture_audio_);
  if (err != kNoError) {
    return err;
  }

  err = echo_cancellation_->ProcessCaptureAudio(capture_audio_);
  if (err != kNoError) {
    return err;
  }

  if (echo_control_mobile_->is_enabled() &&
      noise_suppression_->is_enabled()) {
    capture_audio_->CopyLowPassToReference();
  }

  err = noise_suppression_->ProcessCaptureAudio(capture_audio_);
  if (err != kNoError) {
    return err;
  }

  err = echo_control_mobile_->ProcessCaptureAudio(capture_audio_);
  if (err != kNoError) {
    return err;
  }

  err = voice_detection_->ProcessCaptureAudio(capture_audio_);
  if (err != kNoError) {
    return err;
  }

  err = gain_control_->ProcessCaptureAudio(capture_audio_);
  if (err != kNoError) {
    return err;
  }

  //err = level_estimator_->ProcessCaptureAudio(capture_audio_);
  //if (err != kNoError) {
  //  return err;
  //}

  if (sample_rate_hz_ == kSampleRate32kHz) {
    for (int i = 0; i < num_capture_output_channels_; i++) {
      // Recombine low and high bands.
      SplittingFilterSynthesis(capture_audio_->low_pass_split_data(i),
                               capture_audio_->high_pass_split_data(i),
                               capture_audio_->data(i),
                               capture_audio_->synthesis_filter_state1(i),
                               capture_audio_->synthesis_filter_state2(i));
    }
  }

  capture_audio_->InterleaveTo(frame);

  return kNoError;
}

int AudioProcessingImpl::AnalyzeReverseStream(AudioFrame* frame) {
  CriticalSectionScoped crit_scoped(*crit_);
  int err = kNoError;

  if (frame == NULL) {
    return kNullPointerError;
  }

  if (frame->_frequencyInHz !=
      static_cast<WebRtc_UWord32>(sample_rate_hz_)) {
    return kBadSampleRateError;
  }

  if (frame->_audioChannel != num_render_input_channels_) {
    return kBadNumberChannelsError;
  }

  if (frame->_payloadDataLengthInSamples != samples_per_channel_) {
    return kBadDataLengthError;
  }

  if (debug_file_->Open()) {
    WebRtc_UWord8 event = kRenderEvent;
    if (!debug_file_->Write(&event, sizeof(event))) {
      return kFileError;
    }

    if (!debug_file_->Write(&frame->_frequencyInHz,
                                   sizeof(frame->_frequencyInHz))) {
      return kFileError;
    }

    if (!debug_file_->Write(&frame->_audioChannel,
                                   sizeof(frame->_audioChannel))) {
      return kFileError;
    }

    if (!debug_file_->Write(&frame->_payloadDataLengthInSamples,
        sizeof(frame->_payloadDataLengthInSamples))) {
      return kFileError;
    }

    if (!debug_file_->Write(frame->_payloadData,
        sizeof(WebRtc_Word16) * frame->_payloadDataLengthInSamples *
        frame->_audioChannel)) {
      return kFileError;
    }
  }

  render_audio_->DeinterleaveFrom(frame);

  // TODO(ajm): turn the splitting filter into a component?
  if (sample_rate_hz_ == kSampleRate32kHz) {
    for (int i = 0; i < num_render_input_channels_; i++) {
      // Split into low and high band.
      SplittingFilterAnalysis(render_audio_->data(i),
                              render_audio_->low_pass_split_data(i),
                              render_audio_->high_pass_split_data(i),
                              render_audio_->analysis_filter_state1(i),
                              render_audio_->analysis_filter_state2(i));
    }
  }

  // TODO(ajm): warnings possible from components?
  err = echo_cancellation_->ProcessRenderAudio(render_audio_);
  if (err != kNoError) {
    return err;
  }

  err = echo_control_mobile_->ProcessRenderAudio(render_audio_);
  if (err != kNoError) {
    return err;
  }

  err = gain_control_->ProcessRenderAudio(render_audio_);
  if (err != kNoError) {
    return err;
  }

  //err = level_estimator_->AnalyzeReverseStream(render_audio_);
  //if (err != kNoError) {
  //  return err;
  //}

  was_stream_delay_set_ = false;
  return err;  // TODO(ajm): this is for returning warnings; necessary?
}

int AudioProcessingImpl::set_stream_delay_ms(int delay) {
  was_stream_delay_set_ = true;
  if (delay < 0) {
    return kBadParameterError;
  }

  // TODO(ajm): the max is rather arbitrarily chosen; investigate.
  if (delay > 500) {
    stream_delay_ms_ = 500;
    return kBadStreamParameterWarning;
  }

  stream_delay_ms_ = delay;
  return kNoError;
}

int AudioProcessingImpl::stream_delay_ms() const {
  return stream_delay_ms_;
}

bool AudioProcessingImpl::was_stream_delay_set() const {
  return was_stream_delay_set_;
}

int AudioProcessingImpl::StartDebugRecording(
    const char filename[AudioProcessing::kMaxFilenameSize]) {
  CriticalSectionScoped crit_scoped(*crit_);
  assert(kMaxFilenameSize == FileWrapper::kMaxFileNameSize);

  if (filename == NULL) {
    return kNullPointerError;
  }

  // Stop any ongoing recording.
  if (debug_file_->Open()) {
    if (debug_file_->CloseFile() == -1) {
      return kFileError;
    }
  }

  if (debug_file_->OpenFile(filename, false) == -1) {
    debug_file_->CloseFile();
    return kFileError;
  }

  if (debug_file_->WriteText("%s\n", kMagicNumber) == -1) {
    debug_file_->CloseFile();
    return kFileError;
  }

  // TODO(ajm): should we do this? If so, we need the number of channels etc.
  // Record the default sample rate.
  WebRtc_UWord8 event = kInitializeEvent;
  if (!debug_file_->Write(&event, sizeof(event))) {
    return kFileError;
  }

  if (!debug_file_->Write(&sample_rate_hz_, sizeof(sample_rate_hz_))) {
    return kFileError;
  }

  return kNoError;
}

int AudioProcessingImpl::StopDebugRecording() {
  CriticalSectionScoped crit_scoped(*crit_);
  // We just return if recording hasn't started.
  if (debug_file_->Open()) {
    if (debug_file_->CloseFile() == -1) {
      return kFileError;
    }
  }

  return kNoError;
}

EchoCancellation* AudioProcessingImpl::echo_cancellation() const {
  return echo_cancellation_;
}

EchoControlMobile* AudioProcessingImpl::echo_control_mobile() const {
  return echo_control_mobile_;
}

GainControl* AudioProcessingImpl::gain_control() const {
  return gain_control_;
}

HighPassFilter* AudioProcessingImpl::high_pass_filter() const {
  return high_pass_filter_;
}

LevelEstimator* AudioProcessingImpl::level_estimator() const {
  return level_estimator_;
}

NoiseSuppression* AudioProcessingImpl::noise_suppression() const {
  return noise_suppression_;
}

VoiceDetection* AudioProcessingImpl::voice_detection() const {
  return voice_detection_;
}

WebRtc_Word32 AudioProcessingImpl::Version(WebRtc_Word8* version,
    WebRtc_UWord32& bytes_remaining, WebRtc_UWord32& position) const {
  if (version == NULL) {
    /*WEBRTC_TRACE(webrtc::kTraceError,
               webrtc::kTraceAudioProcessing,
               -1,
               "Null version pointer");*/
    return kNullPointerError;
  }
  memset(&version[position], 0, bytes_remaining);

  WebRtc_Word8 my_version[] = "AudioProcessing 1.0.0";
  // Includes null termination.
  WebRtc_UWord32 length = static_cast<WebRtc_UWord32>(strlen(my_version));
  if (bytes_remaining < length) {
    /*WEBRTC_TRACE(webrtc::kTraceError,
               webrtc::kTraceAudioProcessing,
               -1,
               "Buffer of insufficient length");*/
    return kBadParameterError;
  }
  memcpy(&version[position], my_version, length);
  bytes_remaining -= length;
  position += length;

  std::list<ProcessingComponent*>::const_iterator it;
  for (it = component_list_.begin(); it != component_list_.end(); it++) {
    char component_version[256];
    strcpy(component_version, "\n");
    int err = (*it)->get_version(&component_version[1],
                                 sizeof(component_version) - 1);
    if (err != kNoError) {
      return err;
    }
    if (strncmp(&component_version[1], "\0", 1) == 0) {
      // Assume empty if first byte is NULL.
      continue;
    }

    length = static_cast<WebRtc_UWord32>(strlen(component_version));
    if (bytes_remaining < length) {
      /*WEBRTC_TRACE(webrtc::kTraceError,
                 webrtc::kTraceAudioProcessing,
                 -1,
                 "Buffer of insufficient length");*/
      return kBadParameterError;
    }
    memcpy(&version[position], component_version, length);
    bytes_remaining -= length;
    position += length;
  }

  return kNoError;
}

WebRtc_Word32 AudioProcessingImpl::ChangeUniqueId(const WebRtc_Word32 id) {
  CriticalSectionScoped crit_scoped(*crit_);
  /*WEBRTC_TRACE(webrtc::kTraceModuleCall,
             webrtc::kTraceAudioProcessing,
             id_,
             "ChangeUniqueId(new id = %d)",
             id);*/
  id_ = id;

  return kNoError;
}
}  // namespace webrtc
