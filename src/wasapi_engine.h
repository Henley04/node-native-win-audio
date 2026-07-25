// wasapi_engine.h - WASAPI backend (shared & exclusive modes).
#ifndef NODE_WIN_AUDIO_WASAPI_ENGINE_H_
#define NODE_WIN_AUDIO_WASAPI_ENGINE_H_

// windows.h must come first: ks.h references LONG/ULONG/GUID/HANDLE and will
// fail with C3646/C4430 ("unknown override specifier") if those types aren't
// already defined.  NOMINMAX is also defined globally via binding.gyp; we
// re-assert it here so the header is self-contained when included from a
// translation unit that hasn't seen util.h yet.
//
// We do NOT define WIN32_LEAN_AND_MEAN here: the backends need the full
// windows.h (COM via objbase.h, mmsystem types, etc.) and trimming it broke
// mmsyscom.h / cguid.h / ksmedia.h in various ways.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// objbase.h sets up the COM GUID machinery (DEFINE_GUID / DECLSPEC_UUID /
// __uuidof) that audioclient.h / mmdeviceapi.h / functiondiscoverykeys_*
// rely on.  It is pulled in by windows.h already, but we include it
// explicitly to document the dependency and to keep the header robust should
// someone re-enable WIN32_LEAN_AND_MEAN.
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

// Defensive fallback: on this Windows SDK build (10.0.26100) under
// WIN32_LEAN_AND_MEAN, ksmedia.h defines the individual SPEAKER_FRONT_*
// position macros but skips the composite SPEAKER_MONO / SPEAKER_STEREO /
// SPEAKER_QUAD / SPEAKER_5POINT1 / SPEAKER_7POINT1 macros that
// BuildWaveFormat() needs.  A single outer #ifndef guard does NOT work
// because SPEAKER_FRONT_LEFT is already defined, which would hide the
// missing composites - so every macro gets its own #ifndef.
#ifndef SPEAKER_FRONT_LEFT
#define SPEAKER_FRONT_LEFT             0x1
#endif
#ifndef SPEAKER_FRONT_RIGHT
#define SPEAKER_FRONT_RIGHT            0x2
#endif
#ifndef SPEAKER_FRONT_CENTER
#define SPEAKER_FRONT_CENTER           0x4
#endif
#ifndef SPEAKER_LOW_FREQUENCY
#define SPEAKER_LOW_FREQUENCY          0x8
#endif
#ifndef SPEAKER_BACK_LEFT
#define SPEAKER_BACK_LEFT              0x10
#endif
#ifndef SPEAKER_BACK_RIGHT
#define SPEAKER_BACK_RIGHT             0x20
#endif
#ifndef SPEAKER_SIDE_LEFT
#define SPEAKER_SIDE_LEFT              0x200
#endif
#ifndef SPEAKER_SIDE_RIGHT
#define SPEAKER_SIDE_RIGHT             0x400
#endif
#ifndef SPEAKER_MONO
#define SPEAKER_MONO                   SPEAKER_FRONT_CENTER
#endif
#ifndef SPEAKER_STEREO
#define SPEAKER_STEREO                 (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
#endif
#ifndef SPEAKER_QUAD
#define SPEAKER_QUAD                   (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#endif
#ifndef SPEAKER_5POINT1
#define SPEAKER_5POINT1                (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#endif
#ifndef SPEAKER_7POINT1
#define SPEAKER_7POINT1                (SPEAKER_5POINT1 | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
#endif

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
