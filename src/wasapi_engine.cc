// wasapi_engine.cc - Implementation of the WASAPI backend.
//
// Implements both shared-mode and exclusive-mode WASAPI.  Exclusive mode is
// typically necessary for sub-10ms output latency on consumer hardware.
//
// Strategy:
//   * Shared mode  -> use IAudioClient + event-driven buffer service
//   * Exclusive    -> AUDCLNT_SHAREMODE_EXCLUSIVE with AUDCLNT_STREAMFLAGS_EVENTCALLBACK
//   * The render/capture loop runs on a dedicated thread boosted to "Pro Audio"
//     MMCSS priority.
//   * Sample format conversion is performed if the device format differs from
//     the user-requested format.

#include "wasapi_engine.h"

#include <ks.h>
#include <ksmedia.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>

namespace nwa {

namespace {

constexpr REFERENCE_TIME kRefTimesPerSec = 10000000;  // 100ns units

const CLSID CLSID_MMDeviceEnumerator_ =
    {0xBCDE0395, 0xE52F, 0x4672,
     {0x8E, 0x31, 0x0B, 0x99, 0xB4, 0x5D, 0x3F, 0x6B}};
const IID IID_IMMDeviceEnumerator_ =
    {0xA95664D2, 0x9614, 0x4F35,
     {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x17, 0x7E, 0x99}};
const IID IID_IAudioClient_ =
    {0x1CB9AD4C, 0xDBFA, 0x4C32,
     {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID IID_IAudioRenderClient_ =
    {0xF294ACFC, 0x3146, 0x4483,
     {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const IID IID_IAudioCaptureClient_ =
    {0xC8ADBD64, 0xE71E, 0x48A0,
     {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
const IID IID_IAudioClock_ =
    {0xCD63314F, 0x3FBA, 0x4a1b,
     {0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7}};

// Convert SampleFormat into a WAVEFORMATEXTENSIBLE sub-format GUID.
GUID SubFormatFor(SampleFormat f) {
  switch (f) {
    case SampleFormat::U8:
    case SampleFormat::S16:
    case SampleFormat::S24:
    case SampleFormat::S24_32:
    case SampleFormat::S32: return KSDATAFORMAT_SUBTYPE_PCM;
    case SampleFormat::F32: return KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    case SampleFormat::F64: return KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    default:                return KSDATAFORMAT_SUBTYPE_PCM;
  }
}

void FillPropVariantString(PROPVARIANT* pv, const wchar_t* s) {
  pv->vt = VT_LPWSTR;
  pv->pwszVal = const_cast<wchar_t*>(s);
}

// Create an IMMDeviceEnumerator instance.
HRESULT CreateEnumerator(IMMDeviceEnumerator** out) {
  return CoCreateInstance(CLSID_MMDeviceEnumerator_, nullptr, CLSCTX_ALL,
                          IID_IMMDeviceEnumerator_,
                          reinterpret_cast<void**>(out));
}

}  // namespace

WasapiEngine::WasapiEngine(bool exclusive) : exclusive_(exclusive) {}

WasapiEngine::~WasapiEngine() { Close(); }

WAVEFORMATEXTENSIBLE WasapiEngine::BuildWaveFormat(SampleFormat f,
                                                   uint32_t rate,
                                                   uint16_t channels) const {
  WAVEFORMATEXTENSIBLE wfx{};
  wfx.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
  wfx.Format.nChannels       = channels;
  wfx.Format.nSamplesPerSec  = rate;
  wfx.Format.wBitsPerSample  = static_cast<WORD>(BytesPerSample(f) * 8);
  wfx.Format.nBlockAlign     =
      static_cast<WORD>(wfx.Format.wBitsPerSample / 8 * channels);
  wfx.Format.nAvgBytesPerSec = rate * wfx.Format.nBlockAlign;
  wfx.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  wfx.Samples.wValidBitsPerSample = wfx.Format.wBitsPerSample;
  wfx.dwChannelMask = (channels >= 8) ? SPEAKER_7POINT1
                    : (channels >= 6) ? SPEAKER_5POINT1
                    : (channels >= 4) ? SPEAKER_QUAD
                    : (channels >= 2) ? SPEAKER_STEREO
                    : SPEAKER_MONO;
  wfx.SubFormat = SubFormatFor(f);
  return wfx;
}

HRESULT WasapiEngine::FindDevice(const std::string& id,
                                 StreamDirection dir, IMMDevice** out) {
  IMMDeviceEnumerator* en = nullptr;
  HRESULT hr = CreateEnumerator(&en);
  if (FAILED(hr)) return hr;

  EDataFlow flow = (dir == StreamDirection::Input) ? eCapture : eRender;
  ERole role = eConsole;

  if (id.empty()) {
    hr = en->GetDefaultAudioEndpoint(flow, role, out);
  } else {
    std::wstring wide = Utf8ToWide(id);
    hr = en->GetDevice(wide.c_str(), out);
    if (FAILED(hr)) {
      // Fall back to enumerating and matching by friendly name.
      IMMDeviceCollection* coll = nullptr;
      if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) {
        UINT n = 0;
        coll->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
          IMMDevice* d = nullptr;
          if (SUCCEEDED(coll->Item(i, &d))) {
            IPropertyStore* ps = nullptr;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
              PROPVARIANT pv;
              PropVariantInit(&pv);
              if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) &&
                  pv.vt == VT_LPWSTR) {
                std::string name = WideToUtf8(pv.pwszVal);
                PropVariantClear(&pv);
                if (_stricmp(name.c_str(), id.c_str()) == 0) {
                  *out = d;
                  d = nullptr;
                  ps->Release();
                  coll->Release();
                  en->Release();
                  return S_OK;
                }
              }
              ps->Release();
            }
            if (d) d->Release();
          }
        }
        coll->Release();
      }
      hr = en->GetDefaultAudioEndpoint(flow, role, out);
    }
  }
  en->Release();
  return hr;
}

Status WasapiEngine::EnumerateDevices(StreamDirection dir,
                                      std::vector<DeviceInfo>* out) {
  if (!out) return Status::InvalidArgument;
  out->clear();

  IMMDeviceEnumerator* en = nullptr;
  HRESULT hr = CreateEnumerator(&en);
  if (FAILED(hr)) return HresultToStatus(hr);

  EDataFlow flow = (dir == StreamDirection::Input) ? eCapture : eRender;
  IMMDeviceCollection* coll = nullptr;
  hr = en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll);
  if (FAILED(hr)) { en->Release(); return HresultToStatus(hr); }

  IMMDevice* defDev = nullptr;
  en->GetDefaultAudioEndpoint(flow, eConsole, &defDev);

  UINT n = 0;
  coll->GetCount(&n);
  for (UINT i = 0; i < n; ++i) {
    IMMDevice* d = nullptr;
    if (FAILED(coll->Item(i, &d))) continue;

    LPWSTR idStr = nullptr;
    d->GetId(&idStr);

    DeviceInfo info;
    info.direction = dir;
    if (idStr) {
      info.id = WideToUtf8(idStr);
      CoTaskMemFree(idStr);
    }

    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
      PROPVARIANT pv;
      PropVariantInit(&pv);
      if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) &&
          pv.vt == VT_LPWSTR) {
        info.name = WideToUtf8(pv.pwszVal);
        PropVariantClear(&pv);
      }
      PropVariantInit(&pv);
      if (SUCCEEDED(ps->GetValue(PKEY_DeviceInterface_FriendlyName, &pv)) &&
          pv.vt == VT_LPWSTR) {
        info.adapter = WideToUtf8(pv.pwszVal);
        PropVariantClear(&pv);
      }
      ps->Release();
    }

    if (defDev) {
      LPWSTR defId = nullptr;
      defDev->GetId(&defId);
      if (defId) {
        if (_wcsicmp(defId, Utf8ToWide(info.id).c_str()) == 0) {
          if (dir == StreamDirection::Input)  info.isDefaultInput  = true;
          else                                 info.isDefaultOutput = true;
        }
        CoTaskMemFree(defId);
      }
    }

    // Report common rates/formats as supported.  Actual device formats may
    // differ - IsFormatSupported() performs a real probe.
    info.supportedSampleRates = {44100, 48000, 88200, 96000, 192000};
    info.supportedFormats     = {SampleFormat::S16, SampleFormat::S24,
                                 SampleFormat::S32, SampleFormat::F32};
    info.maxOutputChannels = (dir == StreamDirection::Output) ? 8 : 0;
    info.maxInputChannels  = (dir == StreamDirection::Input)  ? 8 : 0;

    out->push_back(std::move(info));
    d->Release();
  }

  if (defDev) defDev->Release();
  coll->Release();
  en->Release();
  return Status::Ok;
}

Status WasapiEngine::IsFormatSupported(const StreamConfig& cfg,
                                       SampleFormatInfo* nearest) {
  if (cfg.sampleRate == 0 || cfg.channels == 0)
    return Status::InvalidArgument;

  IMMDevice* dev = nullptr;
  HRESULT hr = FindDevice(cfg.deviceId, cfg.direction, &dev);
  if (FAILED(hr)) return HresultToStatus(hr);

  IAudioClient* client = nullptr;
  hr = dev->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr,
                     reinterpret_cast<void**>(&client));
  dev->Release();
  if (FAILED(hr)) return HresultToStatus(hr);

  WAVEFORMATEXTENSIBLE wfx = BuildWaveFormat(cfg.format, cfg.sampleRate,
                                             cfg.channels);
  WAVEFORMATEX* closest = nullptr;
  AUDCLNT_SHAREMODE mode = exclusive_ ? AUDCLNT_SHAREMODE_EXCLUSIVE
                                      : AUDCLNT_SHAREMODE_SHARED;
  hr = client->IsFormatSupported(mode, reinterpret_cast<WAVEFORMATEX*>(&wfx),
                                 reinterpret_cast<WAVEFORMATEX**>(&closest));
  if (closest) {
    if (nearest) {
      nearest->format     = cfg.format;
      nearest->sampleRate = closest->nSamplesPerSec;
      nearest->channels   = closest->nChannels;
    }
    CoTaskMemFree(closest);
  } else if (nearest) {
    nearest->format     = cfg.format;
    nearest->sampleRate = cfg.sampleRate;
    nearest->channels   = cfg.channels;
  }
  client->Release();
  if (hr == S_OK)      return Status::Ok;
  if (hr == S_FALSE)   return Status::FormatNotSupported;  // closest != NULL
  return HresultToStatus(hr);
}

Status WasapiEngine::Open(const StreamConfig& cfg, DataCallback dataCb,
                          ErrorCallback errorCb) {
  if (opened_) return Status::AlreadyInitialized;
  if (cfg.sampleRate == 0 || cfg.channels == 0)
    return Status::InvalidArgument;
  cfg_      = cfg;
  fmt_      = cfg.format;
  channels_ = cfg.channels;
  rate_     = cfg.sampleRate;
  dataCb_   = std::move(dataCb);
  errorCb_  = std::move(errorCb);

  HRESULT hr = FindDevice(cfg_.deviceId, cfg_.direction, &device_);
  if (FAILED(hr)) return HresultToStatus(hr);

  hr = device_->Activate(IID_IAudioClient_, CLSCTX_ALL, nullptr,
                         reinterpret_cast<void**>(&client_));
  if (FAILED(hr)) { Close(); return HresultToStatus(hr); }

  WAVEFORMATEXTENSIBLE wfx = BuildWaveFormat(fmt_, rate_, channels_);

  AUDCLNT_SHAREMODE mode = exclusive_ ? AUDCLNT_SHAREMODE_EXCLUSIVE
                                      : AUDCLNT_SHAREMODE_SHARED;
  DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                AUDCLNT_STREAMFLAGS_NOPERSIST;
  if (cfg_.direction == StreamDirection::Output) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
  if (exclusive_) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;

  // Choose buffer duration.  If the caller asked for explicit frames, convert
  // to REFERENCE_TIME.  Otherwise use ~10ms in shared mode and 3ms in exclusive.
  REFERENCE_TIME duration;
  if (cfg_.bufferFrames > 0) {
    duration = static_cast<REFERENCE_TIME>(kRefTimesPerSec * cfg_.bufferFrames /
                                           rate_);
  } else if (exclusive_) {
    duration = 3 * kRefTimesPerSec / 1000;   // 3 ms
  } else {
    duration = 10 * kRefTimesPerSec / 1000;  // 10 ms
  }
  bufferDuration_ = duration;

  // Get device period for event alignment.
  REFERENCE_TIME defPeriod = 0, minPeriod = 0;
  if (SUCCEEDED(client_->GetDevicePeriod(&defPeriod, &minPeriod))) {
    devicePeriod_ = exclusive_ ? minPeriod : defPeriod;
  } else {
    devicePeriod_ = defPeriod;
  }

  hr = client_->Initialize(mode, flags, duration,
                           exclusive_ ? duration : 0,
                           reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
  if (FAILED(hr)) {
    // Some drivers reject AUTOCONVERTPCM in exclusive mode; retry without it.
    if (exclusive_ && hr == AUDCLNT_E_UNSUPPORTED_FORMAT) {
      flags &= ~AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
      hr = client_->Initialize(mode, flags, duration, duration,
                               reinterpret_cast<WAVEFORMATEX*>(&wfx), nullptr);
    }
    if (FAILED(hr)) { Close(); return HresultToStatus(hr); }
  }

  hr = client_->GetBufferSize(&bufferFrames_);
  if (FAILED(hr)) { Close(); return HresultToStatus(hr); }

  bufferEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!bufferEvent_) { Close(); return Status::BackendError; }
  stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_) { Close(); return Status::BackendError; }

  hr = client_->SetEventHandle(bufferEvent_);
  if (FAILED(hr)) { Close(); return HresultToStatus(hr); }

  if (cfg_.direction == StreamDirection::Output) {
    hr = client_->GetService(IID_IAudioRenderClient_,
                             reinterpret_cast<void**>(&render_));
  } else {
    hr = client_->GetService(IID_IAudioCaptureClient_,
                             reinterpret_cast<void**>(&capture_));
  }
  if (FAILED(hr)) { Close(); return HresultToStatus(hr); }

  client_->GetService(IID_IAudioClock_, reinterpret_cast<void**>(&clock_));

  opened_ = true;
  return Status::Ok;
}

Status WasapiEngine::Start() {
  if (!opened_) return Status::NotInitialized;
  if (running_) return Status::AlreadyRunning;

  ResetEvent(stopEvent_);
  HRESULT hr = client_->Start();
  if (FAILED(hr)) return HresultToStatus(hr);

  running_ = true;
  if (cfg_.direction == StreamDirection::Output) {
    thread_ = std::thread([this] { RenderLoop(); });
  } else {
    thread_ = std::thread([this] { CaptureLoop(); });
  }
  return Status::Ok;
}

Status WasapiEngine::Stop() {
  if (!running_) return Status::NotRunning;
  running_ = false;
  if (stopEvent_) SetEvent(stopEvent_);
  if (thread_.joinable()) thread_.join();
  if (client_) client_->Stop();
  return Status::Ok;
}

Status WasapiEngine::Close() {
  if (running_) Stop();
  if (render_)  { render_->Release();  render_  = nullptr; }
  if (capture_) { capture_->Release(); capture_ = nullptr; }
  if (clock_)   { clock_->Release();   clock_   = nullptr; }
  if (client_)  { client_->Release();  client_  = nullptr; }
  if (device_)  { device_->Release();  device_  = nullptr; }
  if (bufferEvent_) { CloseHandle(bufferEvent_); bufferEvent_ = nullptr; }
  if (stopEvent_)   { CloseHandle(stopEvent_);   stopEvent_   = nullptr; }
  opened_ = false;
  return Status::Ok;
}

Status WasapiEngine::Latency(uint32_t* inputFrames, uint32_t* outputFrames) {
  if (!opened_) return Status::NotInitialized;
  if (cfg_.direction == StreamDirection::Input && inputFrames) {
    UINT32 pad = 0;
    if (SUCCEEDED(capture_->GetNextPacketSize(&pad))) {
      *inputFrames = pad;
    }
  }
  if (cfg_.direction == StreamDirection::Output && outputFrames) {
    UINT32 pad = 0;
    if (SUCCEEDED(client_->GetCurrentPadding(&pad))) {
      // Latency = (buffer - padding) frames in flight + period
      *outputFrames = (bufferFrames_ > pad) ? (bufferFrames_ - pad) : 0;
    }
  }
  return Status::Ok;
}

double WasapiEngine::StreamTimeSeconds() {
  if (!clock_) return -1.0;
  UINT64 pos = 0, freq = 1;
  if (FAILED(clock_->GetFrequency(&freq)) ||
      FAILED(clock_->GetPosition(&pos, nullptr)) || freq == 0)
    return -1.0;
  return static_cast<double>(pos) / static_cast<double>(freq);
}

void WasapiEngine::RenderLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  mmcssTask_ = BeginMmcssThread();

  // Pre-fill the buffer with one period of silence to avoid underruns at start.
  UINT32 padding = 0;
  if (SUCCEEDED(client_->GetCurrentPadding(&padding))) {
    UINT32 toFill = (bufferFrames_ > padding) ? (bufferFrames_ - padding) : 0;
    if (toFill > 0) {
      BYTE* data = nullptr;
      if (SUCCEEDED(render_->GetBuffer(toFill, &data)) && data) {
        // Prime with silence; the first callback will fill subsequent periods.
        std::memset(data, 0, toFill * channels_ * BytesPerSample(fmt_));
        render_->ReleaseBuffer(toFill, 0);
      }
    }
  }

  const size_t frameBytes = channels_ * BytesPerSample(fmt_);
  std::vector<uint8_t> nativeBuf(bufferFrames_ * frameBytes);
  std::vector<float>   floatBuf(bufferFrames_ * channels_);

  HANDLE waits[2] = { stopEvent_, bufferEvent_ };

  while (running_) {
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
    if (w == WAIT_OBJECT_0) break;             // stop
    if (w == WAIT_TIMEOUT) continue;            // occasional wakeup

    UINT32 paddingFrames = 0;
    if (FAILED(client_->GetCurrentPadding(&paddingFrames))) continue;
    UINT32 avail = (bufferFrames_ > paddingFrames)
                     ? (bufferFrames_ - paddingFrames) : 0;
    if (avail == 0) continue;

    BYTE* data = nullptr;
    if (FAILED(render_->GetBuffer(avail, &data)) || !data) continue;

    // Generate audio in float, then convert to device format.
    uint32_t produced = dataCb_(floatBuf.data(), avail);
    if (produced == 0) {
      // Caller had nothing to produce: emit silence.
      std::memset(data, 0, avail * frameBytes);
      render_->ReleaseBuffer(avail, 0);
      continue;
    }
    if (produced < avail) {
      // Tail underrun - zero-fill the remainder.
      std::memset(floatBuf.data() + produced * channels_, 0,
                  (avail - produced) * channels_ * sizeof(float));
      BumpXrun();
    }
    if (fmt_ == SampleFormat::F32) {
      std::memcpy(data, floatBuf.data(), avail * frameBytes);
    } else {
      ConvertFromFloat(floatBuf.data(), avail, channels_, fmt_, data);
    }
    render_->ReleaseBuffer(avail, 0);
  }

  EndMmcssThread(mmcssTask_); mmcssTask_ = nullptr;
  CoUninitialize();
}

void WasapiEngine::CaptureLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  mmcssTask_ = BeginMmcssThread();

  std::vector<float> floatBuf(bufferFrames_ * channels_);

  HANDLE waits[2] = { stopEvent_, bufferEvent_ };

  while (running_) {
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, 200);
    if (w == WAIT_OBJECT_0) break;
    if (w == WAIT_TIMEOUT) continue;

    UINT32 packetFrames = 0;
    while (SUCCEEDED(capture_->GetNextPacketSize(&packetFrames)) &&
           packetFrames > 0 && running_) {
      DWORD flags = 0;
      UINT64 pos = 0;
      BYTE* data = nullptr;
      if (FAILED(capture_->GetBuffer(&data, &packetFrames, &flags, &pos,
                                     nullptr)) || !data) {
        break;
      }
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        std::memset(floatBuf.data(), 0,
                    packetFrames * channels_ * sizeof(float));
        dataCb_(floatBuf.data(), packetFrames);
      } else if (fmt_ == SampleFormat::F32) {
        dataCb_(reinterpret_cast<float*>(data), packetFrames);
      } else {
        ConvertToFloat(data, packetFrames, channels_, fmt_, floatBuf.data());
        dataCb_(floatBuf.data(), packetFrames);
      }
      capture_->ReleaseBuffer(packetFrames);
    }
  }

  EndMmcssThread(mmcssTask_); mmcssTask_ = nullptr;
  CoUninitialize();
}

}  // namespace nwa
