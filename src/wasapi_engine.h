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
// objbase.h must be pulled in before audioclient.h / mmdeviceapi.h /
// functiondiscoverykeys_devpkey.h: WIN32_LEAN_AND_MEAN (defined globally in
// binding.gyp) keeps windows.h from including it, and those headers rely on
// the COM GUID machinery (DEFINE_GUID / DECLSPEC_UUID / __uuidof) that
// objbase.h sets up.  Without it cguid.h fails with
// "error C2059: syntax error: '__uuidof'".
#include <objbase.h>

// KS headers must precede audioclient.h, otherwise ksmedia.h gets pulled in
// transitively (via audioclient.h -> ks.h) before our explicit include, and
// subsequent includes become no-ops due to header guards - leaving
// SPEAKER_* / KSDATAFORMAT_SUBTYPE_* undefined.
//
// mmreg.h is pulled in ahead of ks.h: it carries the WAVEFORMATEXTENSIBLE /
// WAVE_FORMAT_EXTENSIBLE definitions and, on some SDK builds, the
// _SPEAKER_POSITIONS_ block that ksmedia.h otherwise gates out.  Without it
// the SPEAKER_* channel-mask macros stay undefined (C2065) even though
// ksmedia.h is included.
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>  // PKEY_Device_FriendlyName

// Defensive fallback: on some Windows SDK / WIN32_LEAN_AND_MEAN combinations
// ksmedia.h's SPEAKER_* block is skipped, which breaks BuildWaveFormat().
// These are stable Win32 bit masks (from ksmedia.h), so redefining them when
// absent is safe and matches the canonical values.
#ifndef SPEAKER_FRONT_LEFT
#define SPEAKER_FRONT_LEFT             0x1
#define SPEAKER_FRONT_RIGHT            0x2
#define SPEAKER_FRONT_CENTER           0x4
#define SPEAKER_LOW_FREQUENCY          0x8
#define SPEAKER_BACK_LEFT              0x10
#define SPEAKER_BACK_RIGHT             0x20
#define SPEAKER_FRONT_LEFT_OF_CENTER   0x40
#define SPEAKER_FRONT_RIGHT_OF_CENTER  0x80
#define SPEAKER_BACK_CENTER            0x100
#define SPEAKER_SIDE_LEFT              0x200
#define SPEAKER_SIDE_RIGHT             0x400
#define SPEAKER_TOP_CENTER             0x800
#define SPEAKER_TOP_FRONT_LEFT         0x1000
#define SPEAKER_TOP_FRONT_CENTER       0x2000
#define SPEAKER_TOP_FRONT_RIGHT        0x4000
#define SPEAKER_TOP_BACK_LEFT          0x8000
#define SPEAKER_TOP_BACK_CENTER        0x10000
#define SPEAKER_TOP_BACK_RIGHT         0x20000
#define SPEAKER_MONO                   SPEAKER_FRONT_CENTER
#define SPEAKER_STEREO                 (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
#define SPEAKER_2POINT1                (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_LOW_FREQUENCY)
#define SPEAKER_SURROUND               (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_BACK_CENTER)
#define SPEAKER_QUAD                   (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#define SPEAKER_4POINT1                (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#define SPEAKER_5POINT1                (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#define SPEAKER_7POINT1                (SPEAKER_5POINT1 | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
#define SPEAKER_5POINT1_SURROUND       (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
#define SPEAKER_7POINT1_SURROUND       (SPEAKER_5POINT1_SURROUND | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#endif  // SPEAKER_FRONT_LEFT

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
