// wdm_engine.h - WDM Kernel Streaming backend (DirectKS).
//
// Talks directly to WDM audio filter drivers via KS (Kernel Streaming) property
// requests.  This bypasses the DirectSound/MME/WASAPI stacks and historically
// offered very low latency, though modern WASAPI-exclusive matches or exceeds it.
//
// The implementation uses:
//   * SetupAPI to enumerate KS audio filter devices (KSCATEGORY_AUDIO)
//   * KSPROPERTY_PIN_PROPOSEDATAFORMAT + KSPROPERTY_CONNECTION_STATE to set up
//     a streaming pin.
//   * ReadFile/WriteFile on the pin handle in overlapped (async) mode.

#ifndef NODE_WIN_AUDIO_WDM_ENGINE_H_
#define NODE_WIN_AUDIO_WDM_ENGINE_H_

// All Windows SDK header ordering / fallback macro handling lives in
// win_headers.h - include it first.  WDM-KS in particular needs ks.h before
// ksmedia.h (#error otherwise), and ksmedia.h needs mmreg.h to be visible
// first so its KSDATAFORMAT_SUBTYPE_* definitions see WAVEFORMATEXTENSIBLE.
#include "win_headers.h"

#include <atomic>
#include <thread>
#include <vector>

#include "audio_engine.h"
#include "util.h"

namespace nwa {

class WdmEngine : public AudioEngine {
 public:
  WdmEngine() = default;
  ~WdmEngine() override;

  Backend backend() const override { return Backend::Wdm; }
  const char* Name() const override { return "WDM-KS (Kernel Streaming)"; }

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
  // Worker loops.
  void RenderLoop();
  void CaptureLoop();

  // Helper: instantiate a KS pin and set its format / state.
  Status CreatePin(bool render, SampleFormat fmt, uint32_t rate,
                   uint16_t channels, uint32_t bufferFrames);

  // Helper: send a KS property.
  bool KsProperty(const GUID& set, ULONG id, ULONG flags, void* value,
                  ULONG valueSize, ULONG* bytesReturned);

  HANDLE filter_  = INVALID_HANDLE_VALUE;  // filter handle
  HANDLE pin_     = INVALID_HANDLE_VALUE;  // pin handle
  HANDLE stopEvent_  = nullptr;
  HANDLE mmcssTask_  = nullptr;
  std::thread thread_;                      // render / capture worker

  bool opened_  = false;
  bool running_ = false;

  StreamConfig  cfg_;
  SampleFormat  fmt_       = SampleFormat::S16;
  uint16_t      channels_  = 2;
  uint32_t      rate_      = 48000;
  uint32_t      bufferFrames_ = 0;
  uint32_t      numBuffers_   = 4;
  DataCallback  dataCb_;
  ErrorCallback errorCb_;

  std::vector<uint8_t>      allocBuf_;   // pinned I/O buffer
  std::vector<OVERLAPPED>   overlaps_;
  std::vector<HANDLE>       overlapEvents_;

  std::atomic<uint64_t> framesProcessed_{0};
};

}  // namespace nwa

#endif  // NODE_WIN_AUDIO_WDM_ENGINE_H_
