// audio_types.h - Common types shared by all audio backends
#ifndef NODE_WIN_AUDIO_AUDIO_TYPES_H_
#define NODE_WIN_AUDIO_AUDIO_TYPES_H_

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>

namespace nwa {

// Audio sample format (bit depth + representation).
enum class SampleFormat : int {
  Unknown = 0,
  U8,        // 8-bit unsigned PCM
  S16,       // 16-bit signed PCM (little-endian)
  S24,       // 24-bit signed PCM packed (3 bytes per sample)
  S24_32,    // 24-bit signed PCM stored in 4 bytes (MSB padded)
  S32,       // 32-bit signed PCM
  F32,       // 32-bit IEEE float
  F64        // 64-bit IEEE float
};

// Direction of an audio stream.
enum class StreamDirection : int {
  Input  = 0,  // capture
  Output = 1   // render
};

// Result/error codes.  Keep these stable because they are surfaced to JS.
enum class Status : int {
  Ok = 0,
  InvalidArgument,
  NotSupported,
  DeviceNotFound,
  DeviceUnavailable,
  FormatNotSupported,
  AlreadyInitialized,
  NotInitialized,
  AlreadyRunning,
  NotRunning,
  Underrun,
  Overrun,
  BackendError,
  OutOfMemory,
  TimedOut,
  Interrupted,
  PermissionDenied,
  UnknownError
};

// Backend identifier surfaced to JS as `backend` field.
enum class Backend : int {
  Wasapi           = 0,
  WasapiExclusive  = 1,
  Asio             = 2,
  Mme              = 3,
  Wdm              = 4,
  AudioGraph       = 5
};

// Convert a Status to a human-readable string.
const char* StatusToString(Status s);

// Bytes per sample for a given format.  Returns 0 for Unknown.
size_t BytesPerSample(SampleFormat fmt);

// Packed channel layout is implicit (channels are interleaved).
struct SampleFormatInfo {
  SampleFormat format = SampleFormat::Unknown;
  uint32_t sampleRate = 0;        // Hz
  uint16_t channels   = 0;        // 1..N
};

// Description of a single audio endpoint.
struct DeviceInfo {
  std::string id;          // backend-specific stable id
  std::string name;        // human-readable name
  std::string adapter;     // adapter / driver name (optional)
  StreamDirection direction = StreamDirection::Output;
  uint16_t maxInputChannels  = 0;
  uint16_t maxOutputChannels = 0;
  std::vector<uint32_t> supportedSampleRates;
  std::vector<SampleFormat> supportedFormats;
  bool isDefaultInput  = false;
  bool isDefaultOutput = false;
};

// Configuration for opening a stream.
struct StreamConfig {
  std::string deviceId;                  // empty = default device
  StreamDirection direction = StreamDirection::Output;
  SampleFormat format        = SampleFormat::F32;
  uint32_t sampleRate        = 48000;
  uint16_t channels          = 2;
  uint32_t bufferFrames      = 0;        // 0 = backend default
  uint32_t numBuffers        = 0;        // 0 = backend default
  bool exclusive             = false;    // WASAPI exclusive (handled by WasapiExclusive backend)
  bool eventDriven           = true;     // use event-driven mode when backend supports it
  uint32_t requestedLatencyMs = 0;       // 0 = use bufferFrames/numBuffers
};

// Callback invoked when the backend needs more audio (output) or has new audio
// (input).  `frames` may be less than the buffer size on the last call.
// `data` is interleaved PCM at the negotiated format/channels.
// Returns the number of frames actually consumed/produced (== frames on success).
using DataCallback = std::function<uint32_t(float* data, uint32_t frames)>;

// Callback invoked when an error/underrun/overrun happens during streaming.
using ErrorCallback = std::function<void(Status, const std::string&)>;

}  // namespace nwa

#endif  // NODE_WIN_AUDIO_AUDIO_TYPES_H_
