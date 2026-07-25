// audiograph_engine.cc - Implementation of the AudioGraph (WinRT) backend.
//
// Uses C++/WinRT to drive Windows.Media.Audio.AudioGraph.  Because AudioGraph
// is asynchronous at the API level, we use winrt::resume_on (an apartment
// thread) for setup, and the QuantumStarted callback is invoked on the
// graph's own worker thread.

#include "audiograph_engine.h"

// We only build this file when the Windows SDK ships C++/WinRT headers
// (Visual Studio 2019 16.x or newer with the Windows 10 SDK).  If the headers
// are not present we fall back to a stub implementation that reports the
// backend as unsupported.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Media.Render.h>
#include <winrt/Windows.Media.Capture.h>

#include <algorithm>
#include <cstring>
#include <future>

#pragma comment(lib, "windowsapp.lib")

namespace winrt {
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Devices::Enumeration;
using namespace Windows::Media::Audio;
using namespace Windows::Media::MediaProperties;
using namespace Windows::Media::Render;
using namespace Windows::Media::Capture;
}

namespace nwa {

struct AudioGraphEngine::Impl {
  winrt::AudioGraph                graph{nullptr};
  winrt::AudioDeviceOutputNode     outNode{nullptr};
  winrt::AudioDeviceInputNode      inNode{nullptr};
  winrt::AudioFrameOutputNode      frameOut{nullptr};
  winrt::AudioFrameInputNode       frameIn{nullptr};
  winrt::QuantumStartedEventHandler quantumToken{nullptr};
  winrt::InitializeOptions         initOptions = winrt::InitializeOptions::None;
};

namespace {

// Convert our SampleFormat to a WinRT AudioEncodingProperties.  Currently
// AudioGraph only supports 32-bit float, so we always negotiate F32 and do any
// conversion at the addon layer.
winrt::AudioEncodingProperties MakeProperties(uint32_t rate, uint16_t channels) {
  auto p = winrt::AudioEncodingProperties::CreatePcm(rate, channels, 32);
  p.Subtype(L"Float");
  return p;
}

}  // namespace

AudioGraphEngine::~AudioGraphEngine() { Close(); }

Status AudioGraphEngine::EnumerateDevices(StreamDirection dir,
                                          std::vector<DeviceInfo>* out) {
  if (!out) return Status::InvalidArgument;
  out->clear();

  // Find the default render / capture device id via MediaDevice.
  std::wstring defaultId;
  if (dir == StreamDirection::Output) {
    auto s = winrt::MediaDevice::GetDefaultAudioRenderId(
        winrt::AudioRenderRole::Default);
    if (s) defaultId = s.c_str();
  } else {
    auto s = winrt::MediaDevice::GetDefaultAudioCaptureId(
        winrt::AudioCaptureRole::Default);
    if (s) defaultId = s.c_str();
  }

  // Enumerate via DeviceInformation::FindAllAsync with the audio selector.
  auto selector = (dir == StreamDirection::Output)
      ? winrt::MediaDevice::GetAudioRenderSelector()
      : winrt::MediaDevice::GetAudioCaptureSelector();
  auto op = winrt::DeviceInformation::FindAllAsync(
      selector, winrt::IVectorView<winrt::hstring>{}, winrt::DeviceInformationKind::DeviceInterface);
  // Synchronously wait (the addon runs on a background thread; this is OK).
  auto devices = op.get();
  for (auto const& d : devices) {
    DeviceInfo info;
    info.id        = winrt::to_string(d.Id());
    info.name      = winrt::to_string(d.Name());
    info.direction = dir;
    if (dir == StreamDirection::Output) {
      info.maxOutputChannels = 8;
      if (!defaultId.empty() && info.id == WideToUtf8(defaultId.c_str()))
        info.isDefaultOutput = true;
    } else {
      info.maxInputChannels = 8;
      if (!defaultId.empty() && info.id == WideToUtf8(defaultId.c_str()))
        info.isDefaultInput = true;
    }
    info.supportedSampleRates = {44100, 48000, 96000, 192000};
    info.supportedFormats     = {SampleFormat::F32};
    out->push_back(std::move(info));
  }
  return Status::Ok;
}

Status AudioGraphEngine::IsFormatSupported(const StreamConfig& cfg,
                                           SampleFormatInfo* nearest) {
  // AudioGraph only supports 32-bit float PCM.
  if (nearest) {
    nearest->format     = SampleFormat::F32;
    nearest->sampleRate = cfg.sampleRate;
    nearest->channels   = cfg.channels;
  }
  if (cfg.format != SampleFormat::F32) return Status::FormatNotSupported;
  return Status::Ok;
}

Status AudioGraphEngine::Open(const StreamConfig& cfg, DataCallback dataCb,
                              ErrorCallback errorCb) {
  if (opened_) return Status::AlreadyInitialized;
  cfg_      = cfg;
  fmt_      = SampleFormat::F32;       // forced
  channels_ = cfg.channels;
  rate_     = cfg.sampleRate;
  dataCb_   = std::move(dataCb);
  errorCb_  = std::move(errorCb);

  // Initialize the WinRT apartment if not already.  Use multi-threaded.
  try {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
  } catch (...) {
    // Already initialized - ignore.
  }

  impl_ = new Impl{};

  winrt::AudioGraphSettings settings(
      (cfg.direction == StreamDirection::Output)
          ? winrt::AudioRenderCategory::Media
          : winrt::AudioRenderCategory::Media);
  settings.EncodingProperties(MakeProperties(rate_, channels_));
  settings.DesiredRenderDeviceAudioProcessing(
      winrt::AudioProcessing::Raw);   // low latency
  settings.QuantumSizeSelectionMode(
      winrt::QuantumSizeSelectionMode::LowestLatency);

  if (!cfg.deviceId.empty()) {
    auto devId = winrt::to_hstring(cfg.deviceId);
    settings.PrimaryRenderDevice(devId);
  }

  winrt::CreateAudioGraphResult result = nullptr;
  try {
    auto op = winrt::AudioGraph::CreateAsync(settings);
    result = op.get();
  } catch (const winrt::hresult_error& e) {
    delete impl_; impl_ = nullptr;
    errorCb_(Status::BackendError, winrt::to_string(e.message()));
    return Status::BackendError;
  }

  if (result.Status() != winrt::AudioGraphCreationStatus::Success) {
    delete impl_; impl_ = nullptr;
    return Status::BackendError;
  }
  impl_->graph = result.Graph();

  // Create the device output node (render) or input node (capture).
  if (cfg.direction == StreamDirection::Output) {
    auto outOp = impl_->graph.CreateDeviceOutputNodeAsync();
    auto outRes = outOp.get();
    if (outRes.Status() != winrt::AudioDeviceNodeCreationStatus::Success) {
      Close();
      return Status::DeviceUnavailable;
    }
    impl_->outNode = outRes.DeviceOutputNode();

    impl_->frameIn = impl_->graph.CreateFrameInputNode(
        MakeProperties(rate_, channels_));
    impl_->frameIn.AddOutgoingConnection(impl_->outNode);
  } else {
    auto inOp = impl_->graph.CreateDeviceInputNodeAsync();
    auto inRes = inOp.get();
    if (inRes.Status() != winrt::AudioDeviceNodeCreationStatus::Success) {
      Close();
      return Status::DeviceUnavailable;
    }
    impl_->inNode = inRes.DeviceInputNode();
    impl_->frameOut = impl_->graph.CreateFrameOutputNode(
        MakeProperties(rate_, channels_));
    impl_->inNode.AddOutgoingConnection(impl_->frameOut);
  }

  bufferFrames_ = impl_->graph.SamplesPerQuantum();

  // Wire the quantum callback.
  impl_->quantumToken = impl_->graph.QuantumStarted(
      [this](winrt::AudioGraph const& g, winrt::IInspectable const&) -> winrt::IInspectable {
        if (!running_) return nullptr;
        uint32_t frames = g.SamplesPerQuantum();

        if (cfg_.direction == StreamDirection::Output) {
          // Build an AudioFrame of `frames` samples per channel as float.
          winrt::AudioFrame frame(frames * channels_ * sizeof(float));
          {
            auto buf = frame.LockBuffer(winrt::AudioBufferAccessMode::Write);
            auto* p  = reinterpret_cast<float*>(buf.data());
            uint32_t produced = dataCb_(p, frames);
            if (produced < frames) {
              std::memset(p + produced * channels_, 0,
                          (frames - produced) * channels_ * sizeof(float));
              BumpXrun();
            }
          }
          impl_->frameIn.AddFrame(frame);
          framesProcessed_.fetch_add(frames);
        } else {
          auto frame = impl_->frameOut.GetFrame();
          auto buf   = frame.LockBuffer(winrt::AudioBufferAccessMode::Read);
          auto* p    = reinterpret_cast<const float*>(buf.data());
          uint32_t got = buf.Length() / (channels_ * sizeof(float));
          if (got > 0) dataCb_(const_cast<float*>(p), got);
          framesProcessed_.fetch_add(got);
        }
        return nullptr;
      });

  opened_ = true;
  return Status::Ok;
}

Status AudioGraphEngine::Start() {
  if (!opened_) return Status::NotInitialized;
  if (running_) return Status::AlreadyRunning;
  impl_->graph.Start();
  running_ = true;
  return Status::Ok;
}

Status AudioGraphEngine::Stop() {
  if (!running_) return Status::NotRunning;
  impl_->graph.Stop();
  running_ = false;
  return Status::Ok;
}

Status AudioGraphEngine::Close() {
  if (running_) Stop();
  if (impl_) {
    if (impl_->quantumToken) {
      try { impl_->graph.QuantumStarted(impl_->quantumToken); }
      catch (...) {}
      impl_->quantumToken = nullptr;
    }
    impl_->frameIn  = nullptr;
    impl_->frameOut = nullptr;
    impl_->outNode  = nullptr;
    impl_->inNode   = nullptr;
    impl_->graph    = nullptr;
    delete impl_;
    impl_ = nullptr;
  }
  opened_ = false;
  return Status::Ok;
}

Status AudioGraphEngine::Latency(uint32_t* inputFrames, uint32_t* outputFrames) {
  if (!opened_) return Status::NotInitialized;
  uint32_t q = impl_->graph.SamplesPerQuantum();
  if (inputFrames)  *inputFrames  = q;
  if (outputFrames) *outputFrames = q;
  return Status::Ok;
}

double AudioGraphEngine::StreamTimeSeconds() {
  return static_cast<double>(framesProcessed_.load()) /
         static_cast<double>(rate_);
}

}  // namespace nwa
