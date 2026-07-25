// audiograph_engine.h - AudioGraph (WinRT) backend.
//
// Uses Windows.Media.Audio.AudioGraph for low-latency audio on UWP/WinRT.
// AudioGraph is built on WASAPI internally but offers a high-level node graph
// API with frame input/output nodes and per-quantum callbacks.  Latency is
// similar to WASAPI shared mode (~10ms typical).
//
// Requires Windows 10 1809+ and the Windows SDK.  The implementation uses
// C++/WinRT headers from the Windows SDK (no separate NuGet needed when
// targeting recent VS versions).

#ifndef NODE_WIN_AUDIO_AUDIOGRAPH_ENGINE_H_
#define NODE_WIN_AUDIO_AUDIOGRAPH_ENGINE_H_

#include <atomic>
#include <thread>
#include <vector>

#include "audio_engine.h"
#include "util.h"

namespace nwa {

class AudioGraphEngine : public AudioEngine {
 public:
  AudioGraphEngine() = default;
  ~AudioGraphEngine() override;

  Backend backend() const override { return Backend::AudioGraph; }
  const char* Name() const override { return "AudioGraph (WinRT)"; }

  Status EnumerateDevices(StreamDirection dir,
                          std::vector<DeviceInfo>* out) override;
  Status IsFormatSupported(const StreamConfig& cfg,
                           SampleFormatInfo* nearest) override;
  Status Open(const StreamConfig& cfg, DataCallback dataCb,
              ErrorCallback errorCb) override;
  Status Start() override;
  Status Stop() override;
  Status Close() override;
  Status Latency(uint32_t* inputFrames, uint32_t* outputFrames) override;
  double StreamTimeSeconds() override;

 private:
  // Implementation detail hidden in .cc to keep WinRT headers out of the
  // public interface.  This is a pointer to a state struct.
  struct Impl;
  Impl* impl_ = nullptr;

  bool opened_  = false;
  bool running_ = false;
  StreamConfig  cfg_;
  SampleFormat  fmt_       = SampleFormat::F32;
  uint16_t      channels_  = 2;
  uint32_t      rate_      = 48000;
  uint32_t      bufferFrames_ = 0;
  DataCallback  dataCb_;
  ErrorCallback errorCb_;

  std::atomic<uint64_t> framesProcessed_{0};
};

}  // namespace nwa

#endif  // NODE_WIN_AUDIO_AUDIOGRAPH_ENGINE_H_
