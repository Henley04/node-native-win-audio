// asio_engine.h - ASIO backend (Steinberg Audio Streaming Input Output).
//
// ASIO is a Steinberg API used by professional audio interfaces for very low
// latency.  The SDK has restrictive licensing, but the *host* implementation
// that talks to an installed driver can be written from scratch because the
// driver exposes a well-defined IASIO COM-like interface.
//
// Approach:
//   * Enumerate installed ASIO drivers from the registry:
//       HKLM\SOFTWARE\ASIO\<driver name>\CLSID
//   * CoCreateInstance(CLSID, ..., IID_IASIO, ...)
//   * Call Init / GetChannels / GetBufferSize / CreateBuffers / Start.
//   * The driver invokes our bufferSwitch callback on its own thread.
//
// Requires the user to have an ASIO driver installed (typically provided with
// their audio interface).

#ifndef NODE_WIN_AUDIO_ASIO_ENGINE_H_
#define NODE_WIN_AUDIO_ASIO_ENGINE_H_

// All Windows SDK header ordering / fallback macro handling lives in
// win_headers.h - include it first.  ASIO uses CoCreateInstance which needs
// objbase.h's GUID machinery, and our asio_types.h reuses WAVEFORMATEX-ish
// types from mmreg.h.
#include "win_headers.h"

#include <atomic>
#include <thread>
#include <vector>

#include "audio_engine.h"
#include "asio_types.h"
#include "util.h"

namespace nwa {

class AsioEngine : public AudioEngine {
 public:
  AsioEngine() = default;
  ~AsioEngine() override;

  Backend backend() const override { return Backend::Asio; }
  const char* Name() const override { return "ASIO"; }

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
  // Static C-style callbacks invoked by the ASIO driver on its own thread.
  static void CallbackBufferSwitch(long doubleBufferIndex, ASIOBool directProcess);
  static void CallbackSampleRateChanged(ASIOSampleRate sRate);
  static ASIOTime* CallbackBufferSwitchTimeInfo(ASIOTime* params,
                                                long doubleBufferIndex,
                                                ASIOBool directProcess);
  static long CallbackAsioMessage(long selector, long value, void* message,
                                  double* opt);

  void OnBufferSwitch(long doubleBufferIndex);

  bool FindDriverClsid(const std::string& id, std::wstring* clsidWide,
                       std::string* realName);

  IASIO*             asio_      = nullptr;
  bool               opened_    = false;
  bool               running_   = false;
  bool               comInit_   = false;
  HANDLE             mmcssTask_ = nullptr;

  StreamConfig  cfg_;
  SampleFormat  fmt_        = SampleFormat::F32;
  uint16_t      channels_   = 2;
  uint32_t      rate_       = 48000;
  uint32_t      bufferFrames_ = 0;
  uint32_t      numBuffers_   = 2;
  long          inputChannels_  = 0;
  long          outputChannels_ = 0;

  std::vector<ASIOBufferInfo> buffers_;
  ASIOCallbacks callbacks_{};

  DataCallback  dataCb_;
  ErrorCallback errorCb_;
  std::atomic<uint64_t> framesProcessed_{0};

  // Singleton pointer used by the static callbacks - ASIO does not provide a
  // user data pointer.  This limits the addon to a single ASIO stream at a
  // time, which is consistent with most ASIO drivers (one driver = one stream).
  static AsioEngine* activeInstance_;
};

}  // namespace nwa

#endif  // NODE_WIN_AUDIO_ASIO_ENGINE_H_
