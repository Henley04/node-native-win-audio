// audio_engine.h - Common abstract interface that every backend implements.
#ifndef NODE_WIN_AUDIO_AUDIO_ENGINE_H_
#define NODE_WIN_AUDIO_AUDIO_ENGINE_H_

#include "audio_types.h"

namespace nwa {

// Abstract audio engine.  All backends (WASAPI, MME, WDM-KS, ASIO, AudioGraph)
// implement this interface.  The N-API layer selects a concrete implementation
// based on the `backend` field supplied from JS.
class AudioEngine {
 public:
  virtual ~AudioEngine() = default;

  // Backend identifier.
  virtual Backend backend() const = 0;

  // Human-readable name (e.g. "WASAPI (Shared)").
  virtual const char* Name() const = 0;

  // Enumerate devices available to this backend.  Returns Status::Ok on success
  // and fills `out`.  Other status codes indicate a hard backend failure.
  virtual Status EnumerateDevices(StreamDirection dir,
                                  std::vector<DeviceInfo>* out) = 0;

  // Test whether a specific format / rate / channel count is supported.  If the
  // backend cannot probe cheaply it should return Status::Ok and populate the
  // `nearest` config with the closest match.
  virtual Status IsFormatSupported(const StreamConfig& cfg,
                                   SampleFormatInfo* nearest) = 0;

  // Open a stream.  After a successful call the stream is initialized but not
  // yet running; call Start() to begin streaming.
  virtual Status Open(const StreamConfig& cfg,
                      DataCallback dataCb,
                      ErrorCallback errorCb) = 0;

  // Begin streaming.  Must be called after Open().
  virtual Status Start() = 0;

  // Stop streaming but keep the stream open.
  virtual Status Stop() = 0;

  // Close and release all resources.  Safe to call when not open.
  virtual Status Close() = 0;

  // Current latency in frames (sum of input + output hardware + buffer), or
  // Status::NotSupported if the backend cannot report it.
  virtual Status Latency(uint32_t* inputFrames, uint32_t* outputFrames) = 0;

  // Current stream time in seconds (since Start()).  -1.0 if unsupported.
  virtual double StreamTimeSeconds() = 0;

  // Number of underruns / overruns since Start().
  uint32_t XrunCount() const { return xruns_.load(std::memory_order_relaxed); }

 protected:
  void BumpXrun() { xruns_.fetch_add(1, std::memory_order_relaxed); }

  std::atomic<uint32_t> xruns_{0};
};

}  // namespace nwa

#endif  // NODE_WIN_AUDIO_AUDIO_ENGINE_H_
