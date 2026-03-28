// Copyright 2024 LiveKit, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "include/livekit_client/live_kit_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <sys/utsname.h>

#include <flutter/method_channel.h>
#include <flutter/plugin_registrar.h>
#include <flutter/standard_method_codec.h>

#include <flutter_common.h>
#include <flutter_webrtc.h>
#include <flutter_webrtc/flutter_web_r_t_c_plugin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include <pulse/simple.h>
#include <pulse/error.h>

#include "audio_visualizer.h"

#include "task_runner_linux.h"

namespace livekit_client_plugin {

/// Centers the sorted bands by placing higher values in the middle.
std::vector<float> centerBands(const std::vector<float> &sortedBands) {
  std::vector<float> centeredBands(sortedBands.size(), 0);
  size_t leftIndex = sortedBands.size() / 2;
  size_t rightIndex = leftIndex;

  for (size_t index = 0; index < sortedBands.size(); ++index) {
    if (index % 2 == 0) {
      // Place value to the right
      centeredBands[rightIndex] = sortedBands[index];
      rightIndex += 1;
    } else {
      // Place value to the left
      leftIndex -= 1;
      centeredBands[leftIndex] = sortedBands[index];
    }
  }

  return centeredBands;
}

class VisualizerSink : public libwebrtc::AudioTrackSink {
public:
  VisualizerSink(BinaryMessenger *messenger, std::string event_channel_name,
                 libwebrtc::scoped_refptr<libwebrtc::RTCMediaTrack> media_track,
                 bool is_centered = false, int bar_count = 7)
      : channel_(
            std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
                messenger, event_channel_name,
                &flutter::StandardMethodCodec::GetInstance())),
        media_track_(media_track), is_centered_(is_centered),
        bar_count_(bar_count) {
    task_runner_ = std::make_unique<livekit_client_plugin::TaskRunnerLinux>();
    auto handler = std::make_unique<
        flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
        [&](const flutter::EncodableValue *arguments,
            std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>
                &&events)
            -> std::unique_ptr<
                flutter::StreamHandlerError<flutter::EncodableValue>> {
          sink_ = std::move(events);
          std::weak_ptr<flutter::EventSink<flutter::EncodableValue>> weak_sink =
              sink_;
          for (auto &event : event_queue_) {
            PostEvent(event);
          }
          event_queue_.clear();
          on_listen_called_ = true;
          return nullptr;
        },
        [&](const flutter::EncodableValue *arguments)
            -> std::unique_ptr<
                flutter::StreamHandlerError<flutter::EncodableValue>> {
          on_listen_called_ = false;
          return nullptr;
        });

    channel_->SetStreamHandler(std::move(handler));
    audio_visualizer_ =
        std::make_unique<AudioVisualizer>(bar_count_, is_centered_);
    ((libwebrtc::RTCAudioTrack *)media_track_.get())->AddSink(this);
  }
  ~VisualizerSink() override {}

public:
  void OnData(const void *audio_data, int bits_per_sample, int sample_rate,
              size_t number_of_channels, size_t number_of_frames) override {
    if (!on_listen_called_) {
      return;
    }
    std::vector<float> bands;
    if (audio_visualizer_->Process((const int16_t *)audio_data,
                                   (unsigned int)number_of_frames,
                                   float(sample_rate), bands)) {
      // Post the processed data to the event sink
      EncodableList bands_list = EncodableList(bands.begin(), bands.end());
      Success(EncodableValue(bands_list));
    }
  }

  void Success(const flutter::EncodableValue &event, bool cache_event = true) {
    if (on_listen_called_) {
      PostEvent(event);
    } else {
      if (cache_event) {
        event_queue_.push_back(event);
      }
    }
  }

  void PostEvent(const flutter::EncodableValue &event) {
    if (task_runner_) {
      std::weak_ptr<flutter::EventSink<EncodableValue>> weak_sink = sink_;
      task_runner_->EnqueueTask([weak_sink, event]() {
        auto sink = weak_sink.lock();
        if (sink) {
          sink->Success(event);
        }
      });
    } else {
      sink_->Success(event);
    }
  }

  void RemoveSink() {
    ((libwebrtc::RTCAudioTrack *)media_track_.get())->RemoveSink(this);
  }

private:
  std::unique_ptr<AudioVisualizer> audio_visualizer_;
  std::unique_ptr<livekit_client_plugin::TaskRunnerLinux> task_runner_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>> channel_;
  std::shared_ptr<flutter::EventSink<flutter::EncodableValue>> sink_;
  std::list<flutter::EncodableValue> event_queue_;
  bool on_listen_called_ = false;
  libwebrtc::scoped_refptr<libwebrtc::RTCMediaTrack> media_track_;
  bool is_centered_ = false;
  int bar_count_ = 7;
};

/// AudioRendererSink — sends raw Int16 PCM frames to Dart via EventChannel.
/// Also implements a noise gate: computes RMS in OnData(), gates audio via
/// set_enabled(false/true). Sinks still receive real PCM from the source
/// regardless of the enabled state — only the encoder/sender sees silence.
class AudioRendererSink : public libwebrtc::AudioTrackSink {
public:
  AudioRendererSink(BinaryMessenger *messenger,
                    std::string event_channel_name,
                    libwebrtc::scoped_refptr<libwebrtc::RTCMediaTrack> media_track,
                    int target_channels,
                    const std::string &target_format)
      : channel_(
            std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
                messenger, event_channel_name,
                &flutter::StandardMethodCodec::GetInstance())),
        media_track_(media_track),
        target_channels_(target_channels),
        target_format_(target_format) {
    task_runner_ = std::make_unique<livekit_client_plugin::TaskRunnerLinux>();
    auto handler = std::make_unique<
        flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
        [&](const flutter::EncodableValue *arguments,
            std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>
                &&events)
            -> std::unique_ptr<
                flutter::StreamHandlerError<flutter::EncodableValue>> {
          sink_ = std::move(events);
          on_listen_called_ = true;
          return nullptr;
        },
        [&](const flutter::EncodableValue *arguments)
            -> std::unique_ptr<
                flutter::StreamHandlerError<flutter::EncodableValue>> {
          on_listen_called_ = false;
          return nullptr;
        });

    channel_->SetStreamHandler(std::move(handler));
    ((libwebrtc::RTCAudioTrack *)media_track_.get())->AddSink(this);
  }
  ~AudioRendererSink() override {}

  void OnData(const void *audio_data, int bits_per_sample, int sample_rate,
              size_t number_of_channels, size_t number_of_frames) override {
    if (!on_listen_called_) {
      return;
    }

    const int16_t *src = static_cast<const int16_t *>(audio_data);
    size_t src_channels = number_of_channels;
    size_t out_channels = static_cast<size_t>(target_channels_);
    size_t total_samples = number_of_frames * out_channels;

    // Build interleaved Int16 output, reducing channels if needed.
    std::vector<uint8_t> out_bytes(total_samples * 2);
    int16_t *dst = reinterpret_cast<int16_t *>(out_bytes.data());
    for (size_t f = 0; f < number_of_frames; f++) {
      for (size_t c = 0; c < out_channels; c++) {
        size_t src_c = (c < src_channels) ? c : 0;
        dst[f * out_channels + c] = src[f * src_channels + src_c];
      }
    }

    // Noise gate: compute RMS from real audio, then zero the source buffer
    // if gate is closed. We zero the original buffer (via const_cast) so that
    // downstream consumers (encoder/sender) see silence, while we already have
    // the real RMS for monitoring.
    if (gate_enabled_) {
      double sum_squares = 0.0;
      size_t mono_samples = number_of_frames;
      for (size_t f = 0; f < number_of_frames; f++) {
        double s = static_cast<double>(src[f * src_channels]) / 32768.0;
        sum_squares += s * s;
      }
      double rms = std::sqrt(sum_squares / mono_samples);

      auto now = std::chrono::steady_clock::now();
      bool was_open = gate_open_;

      if (rms >= gate_threshold_) {
        last_above_threshold_ = now;
        gate_open_ = true;
      } else if (gate_open_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_above_threshold_).count();
        if (elapsed > gate_hold_ms_) {
          gate_open_ = false;
        }
      }

      // Log every 500th frame for debugging
      gate_frame_count_++;
      if (gate_frame_count_ % 500 == 0) {
        fprintf(stderr, "[NoiseGate] frame#%d rms=%.6f threshold=%.4f gate_open=%d\n",
                gate_frame_count_, rms, gate_threshold_, gate_open_);
      }

      if (gate_open_ != was_open) {
        fprintf(stderr, "[NoiseGate] Gate %s (rms=%.6f threshold=%.4f)\n",
                gate_open_ ? "OPEN" : "CLOSED", rms, gate_threshold_);
      }

      // Zero the source buffer when gate is closed — silences outbound audio
      // while our monitoring (above) already captured the real RMS.
      if (!gate_open_) {
        size_t buffer_size = number_of_frames * number_of_channels * (bits_per_sample / 8);
        memset(const_cast<void*>(audio_data), 0, buffer_size);
        // Also zero our Dart-bound output so the UI sees silence
        memset(out_bytes.data(), 0, out_bytes.size());
      }
    }

    // Build the map that Dart's AudioFrameCaptureNative expects.
    flutter::EncodableMap frame_map;
    frame_map[flutter::EncodableValue("commonFormat")] =
        flutter::EncodableValue(target_format_);
    frame_map[flutter::EncodableValue("sampleRate")] =
        flutter::EncodableValue(sample_rate);
    frame_map[flutter::EncodableValue("channels")] =
        flutter::EncodableValue(target_channels_);
    frame_map[flutter::EncodableValue("data")] =
        flutter::EncodableValue(out_bytes);

    PostEvent(flutter::EncodableValue(frame_map));
  }

  void PostEvent(const flutter::EncodableValue &event) {
    if (task_runner_) {
      std::weak_ptr<flutter::EventSink<EncodableValue>> weak_sink = sink_;
      task_runner_->EnqueueTask([weak_sink, event]() {
        auto sink = weak_sink.lock();
        if (sink) {
          sink->Success(event);
        }
      });
    } else if (sink_) {
      sink_->Success(event);
    }
  }

  void SetGateEnabled(bool enabled, double threshold, int hold_ms) {
    gate_enabled_ = enabled;
    gate_threshold_ = threshold;
    gate_hold_ms_ = hold_ms;
    if (!enabled) {
      gate_open_ = true;
    }
  }

  bool IsGateOpen() const { return gate_open_; }

  void RemoveSink() {
    ((libwebrtc::RTCAudioTrack *)media_track_.get())->RemoveSink(this);
  }

private:
  std::unique_ptr<livekit_client_plugin::TaskRunnerLinux> task_runner_;
  std::unique_ptr<flutter::EventChannel<flutter::EncodableValue>> channel_;
  std::shared_ptr<flutter::EventSink<flutter::EncodableValue>> sink_;
  bool on_listen_called_ = false;
  libwebrtc::scoped_refptr<libwebrtc::RTCMediaTrack> media_track_;
  int target_channels_;
  std::string target_format_;

  // Noise gate state
  bool gate_enabled_ = false;
  double gate_threshold_ = 0.01;
  int gate_hold_ms_ = 300;
  bool gate_open_ = true;
  int gate_frame_count_ = 0;
  std::chrono::steady_clock::time_point last_above_threshold_ =
      std::chrono::steady_clock::now();
};

/// Monitors mic audio via PulseAudio (independent of WebRTC ADM) and controls
/// gate state. When the gate closes, Dart calls set_enabled(false) on the
/// WebRTC track. PulseAudio keeps reading regardless, so the gate can reopen.
class PulseAudioNoiseGate {
public:
  PulseAudioNoiseGate() = default;
  ~PulseAudioNoiseGate() { Stop(); }

  void Start(double threshold, int hold_ms) {
    Stop();
    threshold_ = threshold;
    hold_ms_ = hold_ms;
    gate_open_ = true;
    current_level_ = 0.0;
    running_ = true;
    thread_ = std::thread(&PulseAudioNoiseGate::MonitorLoop, this);
  }

  void Stop() {
    running_ = false;
    if (thread_.joinable()) {
      thread_.join();
    }
    gate_open_ = true;
    current_level_ = 0.0;
  }

  bool IsRunning() const { return running_.load(); }
  bool IsGateOpen() const { return gate_open_.load(); }
  double GetLevel() const { return current_level_.load(); }

private:
  void MonitorLoop() {
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.channels = 1;
    ss.rate = 16000;  // Low rate is fine for level detection

    int error = 0;
    pa_simple *pa = pa_simple_new(
        nullptr,             // default server
        "havok-noisegate",   // app name
        PA_STREAM_RECORD,
        nullptr,             // default device
        "Noise Gate Monitor",
        &ss,
        nullptr,             // default channel map
        nullptr,             // default buffering
        &error);

    if (!pa) {
      fprintf(stderr, "[NoiseGate-PA] Failed to open PulseAudio: %s\n",
              pa_strerror(error));
      running_ = false;
      return;
    }

    fprintf(stderr, "[NoiseGate-PA] Monitor started (threshold=%.4f holdMs=%d)\n",
            threshold_.load(), hold_ms_.load());

    // Read ~10ms chunks at 16kHz mono 16-bit = 160 samples = 320 bytes
    constexpr size_t FRAMES_PER_READ = 160;
    int16_t buffer[FRAMES_PER_READ];
    auto last_above = std::chrono::steady_clock::now();
    int frame_count = 0;

    while (running_.load()) {
      if (pa_simple_read(pa, buffer, sizeof(buffer), &error) < 0) {
        fprintf(stderr, "[NoiseGate-PA] Read error: %s\n", pa_strerror(error));
        break;
      }

      // Compute RMS
      double sum_sq = 0.0;
      for (size_t i = 0; i < FRAMES_PER_READ; i++) {
        double s = buffer[i] / 32768.0;
        sum_sq += s * s;
      }
      double rms = std::sqrt(sum_sq / FRAMES_PER_READ);
      current_level_.store(rms);

      // Skip first ~50 reads for warmup
      frame_count++;
      if (frame_count < 50) continue;

      double thresh = threshold_.load();
      bool was_open = gate_open_.load();
      auto now = std::chrono::steady_clock::now();

      if (rms >= thresh) {
        last_above = now;
        gate_open_.store(true);
      } else if (was_open) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_above).count();
        if (elapsed > hold_ms_.load()) {
          gate_open_.store(false);
        }
      }

      if (gate_open_.load() != was_open) {
        fprintf(stderr, "[NoiseGate-PA] Gate %s (rms=%.6f threshold=%.4f)\n",
                gate_open_.load() ? "OPEN" : "CLOSED", rms, thresh);
      }

      if (frame_count % 100 == 0) {
        fprintf(stderr, "[NoiseGate-PA] #%d rms=%.6f gate=%s\n",
                frame_count, rms, gate_open_.load() ? "open" : "closed");
      }
    }

    pa_simple_free(pa);
    fprintf(stderr, "[NoiseGate-PA] Monitor stopped\n");
  }

  std::atomic<bool> running_{false};
  std::atomic<double> threshold_{0.05};
  std::atomic<int> hold_ms_{300};
  std::atomic<bool> gate_open_{true};
  std::atomic<double> current_level_{0.0};
  std::thread thread_;
};

// Global pointer for FFI access — set once in the plugin constructor.
static PulseAudioNoiseGate* g_noise_gate = nullptr;

class LiveKitPlugin : public flutter::Plugin {
public:
  static void RegisterWithRegistrar(flutter::PluginRegistrar *registrar);

  LiveKitPlugin(BinaryMessenger *messenger);

  virtual ~LiveKitPlugin();

private:
  // Called when a method is called on this plugin's channel from Dart.
  void HandleMethodCall(
      const flutter::MethodCall<flutter::EncodableValue> &method_call,
      std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result);

private:
  flutter_webrtc_plugin::FlutterWebRTC *webrtc_instance_ = nullptr;
  std::unordered_map<std::string, std::unique_ptr<VisualizerSink>> visualizers_;
  std::unordered_map<std::string, std::unique_ptr<AudioRendererSink>> audio_renderers_;
  BinaryMessenger *messenger_ = nullptr;
  mutable std::mutex mutex_;
  PulseAudioNoiseGate noise_gate_;
};

// static
void LiveKitPlugin::RegisterWithRegistrar(flutter::PluginRegistrar *registrar) {
  auto channel =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), "livekit_client",
          &flutter::StandardMethodCodec::GetInstance());

  auto plugin = std::make_unique<LiveKitPlugin>(registrar->messenger());

  channel->SetMethodCallHandler(
      [plugin_pointer = plugin.get()](const auto &call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  registrar->AddPlugin(std::move(plugin));
}

LiveKitPlugin::LiveKitPlugin(BinaryMessenger *messenger)
    : messenger_(messenger) {
  webrtc_instance_ = flutter_webrtc_plugin_get_shared_instance();
  g_noise_gate = &noise_gate_;
}

LiveKitPlugin::~LiveKitPlugin() {}

void LiveKitPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue> &method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name().compare("startVisualizer") == 0) {
    if (!method_call.arguments()) {
      result->Error("Bad Arguments", "Null arguments received");
      return;
    }
    flutter::EncodableMap params =
        GetValue<flutter::EncodableMap>(*method_call.arguments());
    std::string trackId = findString(params, "trackId");
    std::string visualizerId = findString(params, "visualizerId");
    int barCount = findInt(params, "barCount");
    bool isCentered = findBoolean(params, "isCentered");
    if (trackId.empty() || visualizerId.empty()) {
      result->Error("Invalid Arguments",
                    "trackId and visualizerId are required");
      return;
    }
    libwebrtc::scoped_refptr<libwebrtc::RTCMediaTrack> media_track =
        webrtc_instance_->MediaTrackForId(trackId);
    if (!media_track) {
      result->Error("Track Not Found", "No media track found for the given ID");
      return;
    }
    std::ostringstream oss;
    oss << "io.livekit.audio.visualizer/eventChannel-" << trackId << "-"
        << visualizerId;

    mutex_.lock();
    visualizers_[visualizerId] = std::make_unique<VisualizerSink>(
        messenger_, oss.str(), media_track, isCentered, barCount);
    mutex_.unlock();

    result->Success(flutter::EncodableValue(true));
  } else if (method_call.method_name().compare("stopVisualizer") == 0) {
    if (!method_call.arguments()) {
      result->Error("Bad Arguments", "Null arguments received");
      return;
    }
    flutter::EncodableMap args =
        GetValue<flutter::EncodableMap>(*method_call.arguments());
    std::string trackId = findString(args, "trackId");
    std::string visualizerId = findString(args, "visualizerId");
    if (trackId.empty() || visualizerId.empty()) {
      result->Error("Invalid Arguments",
                    "trackId and visualizerId are required");
      return;
    }
    libwebrtc::scoped_refptr<libwebrtc::RTCMediaTrack> media_track =
        webrtc_instance_->MediaTrackForId(trackId);
    if (!media_track) {
      result->Error("Track Not Found", "No media track found for the given ID");
      return;
    }

    mutex_.lock();
    auto it = visualizers_.find(visualizerId);
    if (it != visualizers_.end()) {
      it->second->RemoveSink();
      visualizers_.erase(it);
      mutex_.unlock();
    } else {
      mutex_.unlock();
      result->Error("Visualizer Not Found",
                    "No visualizer found for the given visualizerId");
      return;
    }

    result->Success();
  } else if (method_call.method_name().compare("startAudioRenderer") == 0) {
    if (!method_call.arguments()) {
      result->Error("Bad Arguments", "Null arguments received");
      return;
    }
    flutter::EncodableMap params =
        GetValue<flutter::EncodableMap>(*method_call.arguments());
    std::string trackId = findString(params, "trackId");
    std::string rendererId = findString(params, "rendererId");
    if (trackId.empty() || rendererId.empty()) {
      result->Error("Invalid Arguments",
                    "trackId and rendererId are required");
      return;
    }

    int targetChannels = 1;
    std::string targetFormat = "int16";
    auto formatIt = params.find(flutter::EncodableValue("format"));
    if (formatIt != params.end()) {
      auto formatMap = std::get<flutter::EncodableMap>(formatIt->second);
      auto chIt = formatMap.find(flutter::EncodableValue("channels"));
      if (chIt != formatMap.end()) {
        targetChannels = std::get<int>(chIt->second);
      }
      auto fmtIt = formatMap.find(flutter::EncodableValue("commonFormat"));
      if (fmtIt != formatMap.end()) {
        targetFormat = std::get<std::string>(fmtIt->second);
      }
    }

    libwebrtc::scoped_refptr<libwebrtc::RTCMediaTrack> media_track =
        webrtc_instance_->MediaTrackForId(trackId);
    if (!media_track) {
      result->Error("Track Not Found",
                    "No media track found for the given ID");
      return;
    }

    std::ostringstream oss;
    oss << "io.livekit.audio.renderer/channel-" << rendererId;

    mutex_.lock();
    audio_renderers_[rendererId] = std::make_unique<AudioRendererSink>(
        messenger_, oss.str(), media_track, targetChannels, targetFormat);
    mutex_.unlock();

    result->Success(flutter::EncodableValue(true));
  } else if (method_call.method_name().compare("setNoiseGate") == 0) {
    if (!method_call.arguments()) {
      result->Error("Bad Arguments", "Null arguments received");
      return;
    }
    flutter::EncodableMap params =
        GetValue<flutter::EncodableMap>(*method_call.arguments());
    std::string rendererId = findString(params, "rendererId");
    bool enabled = findBoolean(params, "enabled");
    double threshold = 0.01;
    int holdMs = 300;
    auto thIt = params.find(flutter::EncodableValue("threshold"));
    if (thIt != params.end()) {
      threshold = std::get<double>(thIt->second);
    }
    auto holdIt = params.find(flutter::EncodableValue("holdMs"));
    if (holdIt != params.end()) {
      // Dart int arrives as int32_t or int64_t depending on value size.
      if (auto *p = std::get_if<int32_t>(&holdIt->second)) {
        holdMs = *p;
      } else if (auto *p = std::get_if<int64_t>(&holdIt->second)) {
        holdMs = static_cast<int>(*p);
      }
    }

    fprintf(stderr, "[NoiseGate] setNoiseGate: rendererId=%s enabled=%d threshold=%f holdMs=%d\n",
            rendererId.c_str(), enabled, threshold, holdMs);

    mutex_.lock();
    auto it = audio_renderers_.find(rendererId);
    if (it != audio_renderers_.end()) {
      it->second->SetGateEnabled(enabled, threshold, holdMs);
      mutex_.unlock();
      fprintf(stderr, "[NoiseGate] Gate configured successfully\n");
    } else {
      mutex_.unlock();
      fprintf(stderr, "[NoiseGate] ERROR: Renderer not found for id: %s\n", rendererId.c_str());
      result->Error("Renderer Not Found",
                    "No audio renderer found for the given rendererId");
      return;
    }

    result->Success();
  } else if (method_call.method_name().compare("startPulseNoiseGate") == 0) {
    if (!method_call.arguments()) {
      result->Error("Bad Arguments", "Null arguments received");
      return;
    }
    flutter::EncodableMap params =
        GetValue<flutter::EncodableMap>(*method_call.arguments());
    double threshold = 0.05;
    int holdMs = 300;
    auto thIt = params.find(flutter::EncodableValue("threshold"));
    if (thIt != params.end()) {
      threshold = std::get<double>(thIt->second);
    }
    auto holdIt = params.find(flutter::EncodableValue("holdMs"));
    if (holdIt != params.end()) {
      if (auto *p = std::get_if<int32_t>(&holdIt->second)) {
        holdMs = *p;
      } else if (auto *p64 = std::get_if<int64_t>(&holdIt->second)) {
        holdMs = static_cast<int>(*p64);
      }
    }
    noise_gate_.Start(threshold, holdMs);
    result->Success();
  } else if (method_call.method_name().compare("stopPulseNoiseGate") == 0) {
    noise_gate_.Stop();
    result->Success();
  } else if (method_call.method_name().compare("getPulseNoiseGateState") == 0) {
    flutter::EncodableMap state;
    state[flutter::EncodableValue("gateOpen")] =
        flutter::EncodableValue(noise_gate_.IsGateOpen());
    state[flutter::EncodableValue("level")] =
        flutter::EncodableValue(noise_gate_.GetLevel());
    result->Success(flutter::EncodableValue(state));
  } else if (method_call.method_name().compare("stopAudioRenderer") == 0) {
    if (!method_call.arguments()) {
      result->Error("Bad Arguments", "Null arguments received");
      return;
    }
    flutter::EncodableMap args =
        GetValue<flutter::EncodableMap>(*method_call.arguments());
    std::string rendererId = findString(args, "rendererId");
    if (rendererId.empty()) {
      result->Error("Invalid Arguments", "rendererId is required");
      return;
    }

    mutex_.lock();
    auto it = audio_renderers_.find(rendererId);
    if (it != audio_renderers_.end()) {
      it->second->RemoveSink();
      audio_renderers_.erase(it);
      mutex_.unlock();
    } else {
      mutex_.unlock();
      result->Error("Renderer Not Found",
                    "No audio renderer found for the given rendererId");
      return;
    }

    result->Success();
  } else {
    result->NotImplemented();
  }
}

} // namespace livekit_client_plugin

void live_kit_plugin_register_with_registrar(FlPluginRegistrar *registrar) {
  static auto *plugin_registrar = new flutter::PluginRegistrar(registrar);
  livekit_client_plugin::LiveKitPlugin::RegisterWithRegistrar(plugin_registrar);
}

// FFI exports — Dart reads these directly via dart:ffi, bypassing the platform channel.
extern "C" __attribute__((visibility("default")))
bool pulse_noise_gate_is_open() {
  return livekit_client_plugin::g_noise_gate
      ? livekit_client_plugin::g_noise_gate->IsGateOpen()
      : true;
}

extern "C" __attribute__((visibility("default")))
double pulse_noise_gate_get_level() {
  return livekit_client_plugin::g_noise_gate
      ? livekit_client_plugin::g_noise_gate->GetLevel()
      : 0.0;
}