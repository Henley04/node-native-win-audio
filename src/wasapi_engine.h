// wasapi_engine.h - WASAPI backend (shared & exclusive modes).
#ifndef NODE_WIN_AUDIO_WASAPI_ENGINE_H_
#define NODE_WIN_AUDIO_WASAPI_ENGINE_H_

// windows.h must come first: ks.h references LONG/ULONG/GUID/HANDLE and will
// fail with C3646/C4430 ("unknown override specifier") if those types aren't
// already defined.  NOMINMAX / WIN32_LEAN_AND_MEAN are also defined globally
// via binding.gyp, but we re-assert them here so the header is self-contained
// when included from a translation unit that hasn't seen util.h yet.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// KS headers must precede audioclient.h, otherwise ksmedia.h gets pulled in
// transitively (via audioclient.h -> ks.h) before our explicit include, and
// subsequent includes become no-ops due to header guards - leaving
// SPEAKER_* / KSDATAFORMAT_SUBTYPE_* undefined.
#include <ks.h>
#include <ksmedia.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>  // PKEY_Device_FriendlyName

#include <atomic>
#include <thread>

#include "audio_engine.h"
#include "util.h"

namespace nwa {

class WasapiEngine : public AudioEngine {
 public:
  // If `exclusive` is true the engine will open in AUDCLNT_SHAREMODE_EXCLUSIVE.
  explicit WasapiEngine(bool exclusive);
  ~WasapiEngine() override;

  Backend backend() const override {
    return exclusive_ ? Backend::WasapiExclusive : Backend::Wasapi;
  }
  const char* Name() const override {
    return exclusive_ ? "WASAPI (Exclusive)" : "WASAPI (Shared)";
  }

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
  // Worker thread loop (event driven).
  void RenderLoop();
  void CaptureLoop();

  // Build a WAVEFORMATEXTENSIBLE for the requested config.
  WAVEFORMATEXTENSIBLE BuildWaveFormat(SampleFormat f, uint32_t rate,
                                       uint16_t channels) const;

  // Find a device by id (empty = default).
  HRESULT FindDevice(const std::string& id, StreamDirection dir,
                     IMMDevice** out);

  bool exclusive_ = false;
  bool running_   = false;
  bool opened_    = false;

  // COM objects owned by the worker thread.
  IMMDevice*           device_      = nullptr;
  IAudioClient*        client_      = nullptr;
  IAudioRenderClient*  render_      = nullptr;
  IAudioCaptureClient* capture_     = nullptr;
  IAudioClock*         clock_       = nullptr;
  HANDLE               bufferEvent_ = nullptr;
  HANDLE               stopEvent_   = nullptr;

  StreamConfig   cfg_;
  SampleFormat   fmt_        = SampleFormat::F32;
  uint16_t       channels_   = 2;
  uint32_t       rate_       = 48000;
  uint32_t       bufferFrames_ = 0;
  DataCallback   dataCb_;
  ErrorCallback  errorCb_;

  std::thread       thread_;
  HANDLE            mmcssTask_ = nullptr;
  REFERENCE_TIME    devicePeriod_ = 0;
  REFERENCE_TIME    bufferDuration_ = 0;
};

}  // namespace nwa

#endif  // NODE_WIN_AUDIO_WASAPI_ENGINE_H_
