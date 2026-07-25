// wdm_engine.cc - Implementation of the WDM-KS backend.
//
// Uses the DirectKS approach documented in MSDN: enumerate filter factories
// exposed by KSCATEGORY_AUDIO devices via SetupAPI, open a pin that supports
// the requested data flow (KSPIN_DATAFLOW_IN for render, OUT for capture),
// set its data format with KSPROPERTY_PIN_PROPOSEDATAFORMAT, and stream
// via overlapped ReadFile/WriteFile.
//
// This is a substantial simplification of full DirectKS.  Pin factory
// enumeration is performed lazily by trying pin IDs 0..N until one accepts
// the proposed format.  This works on the vast majority of WDM drivers but
// may miss pins that require explicit KSPROPERTY_PIN_PHYSICALCONNECTIONS
// chains.

// initguid.h MUST be included before any header that uses DEFINE_GUID
// (ks.h, ksmedia.h) - it redefines DEFINE_GUID to emit an actual definition
// rather than an extern declaration.  win_headers.h (pulled in by
// wdm_engine.h) includes ks.h / ksmedia.h, so initguid.h has to come first.
#include <initguid.h>

#include "wdm_engine.h"

#include <setupapi.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ksuser.lib")

namespace nwa {

namespace {

// KSCATEGORY_AUDIO GUID - copied here to avoid linker dependency on ksuser
// when not present.
// {6994AD04-93EF-11D0-A3CC-00A0C9223196}
const GUID KSCATEGORY_AUDIO_ = {
  0x6994AD04, 0x93EF, 0x11D0,
  {0xA3, 0xCC, 0x00, 0xA0, 0xC9, 0x22, 0x31, 0x96}};

// Build a KSDATAFORMAT_WAVEFORMATEX for the requested PCM/float format.
void BuildKsDataFormat(SampleFormat f, uint32_t rate, uint16_t channels,
                       KSDATAFORMAT_WAVEFORMATEX* out) {
  WORD bits = static_cast<WORD>(BytesPerSample(f) * 8);
  GUID sub  = (f == SampleFormat::F32 || f == SampleFormat::F64)
                ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                : KSDATAFORMAT_SUBTYPE_PCM;

  out->DataFormat.FormatSize   = sizeof(KSDATAFORMAT_WAVEFORMATEX);
  out->DataFormat.Flags        = 0;
  out->DataFormat.SampleSize   = bits / 8 * channels;
  out->DataFormat.Reserved     = 0;
  out->DataFormat.MajorFormat  = KSDATAFORMAT_TYPE_AUDIO;
  out->DataFormat.SubFormat    = sub;
  out->DataFormat.Specifier    = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

  out->WaveFormatEx.wFormatTag      = WAVE_FORMAT_PCM;
  out->WaveFormatEx.nChannels       = channels;
  out->WaveFormatEx.nSamplesPerSec  = rate;
  out->WaveFormatEx.wBitsPerSample  = bits;
  out->WaveFormatEx.nBlockAlign     = static_cast<WORD>(bits / 8 * channels);
  out->WaveFormatEx.nAvgBytesPerSec = rate * (bits / 8 * channels);
  out->WaveFormatEx.cbSize          = 0;
}

// Find filter device path matching friendly name (or first match when empty).
bool FindFilterPath(const std::string& id, std::wstring* outPath,
                    std::string* outName) {
  HDEVINFO hSet = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO_, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (hSet == INVALID_HANDLE_VALUE) return false;

  SP_DEVICE_INTERFACE_DATA did{};
  did.cbSize = sizeof(did);
  bool found = false;
  for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hSet, nullptr,
                                                &KSCATEGORY_AUDIO_, i, &did);
       ++i) {
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(hSet, &did, nullptr, 0, &needed, nullptr);
    if (needed == 0) continue;

    std::vector<uint8_t> buf(needed);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(hSet, &did, detail, needed, nullptr,
                                          nullptr)) continue;

    std::wstring path(detail->DevicePath);
    // Try to read friendly name from the device registry.
    SP_DEVINFO_DATA devInfo{};
    devInfo.cbSize = sizeof(devInfo);
    if (!SetupDiEnumDeviceInfo(hSet, i, &devInfo)) continue;

    wchar_t nameBuf[256] = {0};
    SetupDiGetDeviceRegistryPropertyW(hSet, &devInfo, SPDRP_DEVICEDESC, nullptr,
                                      reinterpret_cast<PBYTE>(nameBuf),
                                      sizeof(nameBuf) - sizeof(wchar_t), nullptr);
    std::string name = WideToUtf8(nameBuf);

    if (id.empty() ||
        _stricmp(name.c_str(), id.c_str()) == 0 ||
        wcsstr(path.c_str(), Utf8ToWide(id).c_str()) != nullptr) {
      *outPath = path;
      *outName = name;
      found = true;
      break;
    }
  }
  SetupDiDestroyDeviceInfoList(hSet);
  return found;
}

}  // namespace

WdmEngine::~WdmEngine() { Close(); }

Status WdmEngine::EnumerateDevices(StreamDirection dir,
                                   std::vector<DeviceInfo>* out) {
  if (!out) return Status::InvalidArgument;
  out->clear();

  HDEVINFO hSet = SetupDiGetClassDevsW(&KSCATEGORY_AUDIO_, nullptr, nullptr,
                                       DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (hSet == INVALID_HANDLE_VALUE) return Status::BackendError;

  SP_DEVICE_INTERFACE_DATA did{};
  did.cbSize = sizeof(did);
  for (DWORD i = 0; SetupDiEnumDeviceInterfaces(hSet, nullptr,
                                                &KSCATEGORY_AUDIO_, i, &did);
       ++i) {
    DWORD needed = 0;
    SetupDiGetDeviceInterfaceDetailW(hSet, &did, nullptr, 0, &needed, nullptr);
    if (needed == 0) continue;
    std::vector<uint8_t> buf(needed);
    auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(hSet, &did, detail, needed, nullptr,
                                          nullptr)) continue;

    SP_DEVINFO_DATA devInfo{};
    devInfo.cbSize = sizeof(devInfo);
    if (!SetupDiEnumDeviceInfo(hSet, i, &devInfo)) continue;

    wchar_t nameBuf[256] = {0};
    SetupDiGetDeviceRegistryPropertyW(hSet, &devInfo, SPDRP_DEVICEDESC, nullptr,
                                      reinterpret_cast<PBYTE>(nameBuf),
                                      sizeof(nameBuf) - sizeof(wchar_t), nullptr);

    DeviceInfo info;
    info.id        = WideToUtf8(detail->DevicePath);
    info.name      = WideToUtf8(nameBuf);
    info.direction = dir;
    // KS drivers can do both directions; report the requested direction's max
    // channels conservatively.
    if (dir == StreamDirection::Output) info.maxOutputChannels = 8;
    else                                info.maxInputChannels  = 8;
    info.supportedSampleRates = {44100, 48000, 88200, 96000, 192000};
    info.supportedFormats     = {SampleFormat::S16, SampleFormat::S24,
                                 SampleFormat::S32, SampleFormat::F32};
    out->push_back(std::move(info));
  }

  SetupDiDestroyDeviceInfoList(hSet);
  return Status::Ok;
}

Status WdmEngine::IsFormatSupported(const StreamConfig& cfg,
                                    SampleFormatInfo* nearest) {
  if (nearest) {
    nearest->format     = cfg.format;
    nearest->sampleRate = cfg.sampleRate;
    nearest->channels   = cfg.channels;
  }
  // Real probing requires opening the filter and querying each pin factory.
  // We optimistically report support - Open() will reject if it can't.
  return Status::Ok;
}

bool WdmEngine::KsProperty(const GUID& set, ULONG id, ULONG flags,
                           void* value, ULONG valueSize,
                           ULONG* bytesReturned) {
  KSPROPERTY prop{};
  prop.Set   = set;
  prop.Id    = id;
  prop.Flags = flags;

  DWORD br = 0;
  BOOL ok = DeviceIoControl(pin_ != INVALID_HANDLE_VALUE ? pin_ : filter_,
                            IOCTL_KS_PROPERTY, &prop, sizeof(prop),
                            value, valueSize, &br, nullptr);
  if (bytesReturned) *bytesReturned = br;
  return ok && br > 0;
}

// Query a pin-factory scoped property (uses KSP_PIN with PinId filled in).
bool WdmEngine_KsPinProperty(HANDLE filter, const GUID& set, ULONG id,
                             ULONG flags, ULONG pinId,
                             void* value, ULONG valueSize,
                             ULONG* bytesReturned) {
  KSP_PIN prop{};
  prop.Property.Set    = set;
  prop.Property.Id     = id;
  prop.Property.Flags  = flags;
  prop.PinId           = pinId;
  prop.Reserved        = 0;

  DWORD br = 0;
  BOOL ok = DeviceIoControl(filter, IOCTL_KS_PROPERTY,
                            &prop, sizeof(prop),
                            value, valueSize, &br, nullptr);
  if (bytesReturned) *bytesReturned = br;
  return ok && br > 0;
}

Status WdmEngine::CreatePin(bool render, SampleFormat fmt, uint32_t rate,
                            uint16_t channels, uint32_t bufferFrames) {
  if (filter_ == INVALID_HANDLE_VALUE) return Status::NotInitialized;

  KSDATAFORMAT_WAVEFORMATEX desired{};
  BuildKsDataFormat(fmt, rate, channels, &desired);

  // Try pin factories 0..15 until one accepts our format.
  for (ULONG pinId = 0; pinId < 16; ++pinId) {
    KSPIN_DATAFLOW flow = KSPIN_DATAFLOW_IN;
    if (!WdmEngine_KsPinProperty(filter_, KSPROPSETID_Pin,
                                 KSPROPERTY_PIN_DATAFLOW,
                                 KSPROPERTY_TYPE_GET, pinId,
                                 &flow, sizeof(flow), nullptr)) {
      continue;
    }
    bool flowOk = render ? (flow == KSPIN_DATAFLOW_IN)
                         : (flow == KSPIN_DATAFLOW_OUT);
    if (!flowOk) continue;

    // Try to set the proposed data format on this pin factory.
    DWORD br = 0;
    BOOL ok = WdmEngine_KsPinProperty(filter_, KSPROPSETID_Pin,
                                      KSPROPERTY_PIN_PROPOSEDATAFORMAT,
                                      KSPROPERTY_TYPE_SET, pinId,
                                      &desired, sizeof(desired), &br);
    if (!ok) continue;

    // Build the connection descriptor.  KsCreatePin expects a KSPIN_CONNECT
    // immediately followed by a KSDATAFORMAT describing the desired format.
    struct PinConnectBundle {
      KSPIN_CONNECT          connect;
      KSDATAFORMAT_WAVEFORMATEX format;
    };
    PinConnectBundle bundle{};
    bundle.connect.Interface.Set        = KSINTERFACESETID_Standard;
    bundle.connect.Interface.Id         = KSINTERFACE_STANDARD_STREAMING;
    bundle.connect.Interface.Flags      = 0;
    bundle.connect.Medium.Set           = KSMEDIUMSETID_Standard;
    bundle.connect.Medium.Id            = 0;
    bundle.connect.Medium.Flags         = 0;
    bundle.connect.PinId                = pinId;
    bundle.connect.PinToHandle          = nullptr;
    bundle.connect.Priority.PriorityClass    = KSPRIORITY_NORMAL;
    bundle.connect.Priority.PrioritySubClass = 1;
    bundle.format                       = desired;

    HANDLE pin = INVALID_HANDLE_VALUE;
    DWORD access = render ? GENERIC_WRITE : GENERIC_READ;

    HRESULT hr = KsCreatePin(filter_, &bundle.connect, access, &pin);
    if (SUCCEEDED(hr) && pin != INVALID_HANDLE_VALUE) {
      pin_ = pin;
      return Status::Ok;
    }
  }
  return Status::FormatNotSupported;
}

Status WdmEngine::Open(const StreamConfig& cfg, DataCallback dataCb,
                       ErrorCallback errorCb) {
  if (opened_) return Status::AlreadyInitialized;
  cfg_      = cfg;
  fmt_      = cfg.format;
  channels_ = cfg.channels;
  rate_     = cfg.sampleRate;
  dataCb_   = std::move(dataCb);
  errorCb_  = std::move(errorCb);

  std::wstring path;
  std::string  name;
  if (!FindFilterPath(cfg.deviceId, &path, &name))
    return Status::DeviceNotFound;

  filter_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                        nullptr);
  if (filter_ == INVALID_HANDLE_VALUE) return Status::DeviceUnavailable;

  bufferFrames_ = cfg.bufferFrames > 0 ? cfg.bufferFrames : 256;
  numBuffers_   = cfg.numBuffers   > 0 ? cfg.numBuffers   : 4;

  bool render = (cfg.direction == StreamDirection::Output);
  Status s = CreatePin(render, fmt_, rate_, channels_, bufferFrames_);
  if (s != Status::Ok) { Close(); return s; }

  // Set pin state to ACQUIRE first (KSSTATE_RUN is set in Start()).
  KSSTATE state = KSSTATE_ACQUIRE;
  if (!KsProperty(KSPROPSETID_Connection,
                  KSPROPERTY_CONNECTION_STATE, KSPROPERTY_TYPE_SET,
                  &state, sizeof(state), nullptr)) {
    Close(); return Status::BackendError;
  }

  size_t frameBytes  = channels_ * BytesPerSample(fmt_);
  size_t bufBytes    = bufferFrames_ * frameBytes;
  allocBuf_.assign(bufBytes * numBuffers_, 0);
  overlaps_.resize(numBuffers_);
  overlapEvents_.resize(numBuffers_);
  for (uint32_t i = 0; i < numBuffers_; ++i) {
    overlapEvents_[i] = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    overlaps_[i] = {};
    overlaps_[i].hEvent = overlapEvents_[i];
  }

  stopEvent_  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_) { Close(); return Status::BackendError; }

  opened_ = true;
  return Status::Ok;
}

Status WdmEngine::Start() {
  if (!opened_) return Status::NotInitialized;
  if (running_) return Status::AlreadyRunning;

  KSSTATE run = KSSTATE_RUN;
  if (!KsProperty(KSPROPSETID_Connection,
                  KSPROPERTY_CONNECTION_STATE, KSPROPERTY_TYPE_SET,
                  &run, sizeof(run), nullptr)) {
    return Status::BackendError;
  }

  running_ = true;
  ResetEvent(stopEvent_);
  if (cfg_.direction == StreamDirection::Output)
    thread_ = std::thread([this] { RenderLoop(); });
  else
    thread_ = std::thread([this] { CaptureLoop(); });
  return Status::Ok;
}

Status WdmEngine::Stop() {
  if (!running_) return Status::NotRunning;
  running_ = false;
  SetEvent(stopEvent_);
  if (thread_.joinable()) thread_.join();
  KSSTATE stop = KSSTATE_PAUSE;
  KsProperty(KSPROPSETID_Connection,
             KSPROPERTY_CONNECTION_STATE, KSPROPERTY_TYPE_SET,
             &stop, sizeof(stop), nullptr);
  return Status::Ok;
}

Status WdmEngine::Close() {
  if (running_) Stop();
  for (HANDLE h : overlapEvents_) if (h) CloseHandle(h);
  overlapEvents_.clear();
  overlaps_.clear();
  allocBuf_.clear();
  if (pin_ != INVALID_HANDLE_VALUE) {
    KSSTATE stop = KSSTATE_STOP;
    KsProperty(KSPROPSETID_Connection,
               KSPROPERTY_CONNECTION_STATE, KSPROPERTY_TYPE_SET,
               &stop, sizeof(stop), nullptr);
    CloseHandle(pin_); pin_ = INVALID_HANDLE_VALUE;
  }
  if (filter_ != INVALID_HANDLE_VALUE) {
    CloseHandle(filter_); filter_ = INVALID_HANDLE_VALUE;
  }
  if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
  opened_ = false;
  return Status::Ok;
}

Status WdmEngine::Latency(uint32_t* inputFrames, uint32_t* outputFrames) {
  if (inputFrames)  *inputFrames  = bufferFrames_ * numBuffers_;
  if (outputFrames) *outputFrames = bufferFrames_ * numBuffers_;
  return Status::Ok;
}

double WdmEngine::StreamTimeSeconds() {
  return static_cast<double>(framesProcessed_.load()) /
         static_cast<double>(rate_);
}

void WdmEngine::RenderLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  mmcssTask_ = BeginMmcssThread(L"Pro Audio");

  size_t frameBytes = channels_ * BytesPerSample(fmt_);
  size_t bufBytes   = bufferFrames_ * frameBytes;
  std::vector<float> floatBuf(bufferFrames_ * channels_);

  // Submit all buffers initially.
  for (uint32_t i = 0; i < numBuffers_; ++i) {
    uint32_t produced = dataCb_(floatBuf.data(), bufferFrames_);
    if (produced < bufferFrames_) {
      std::memset(floatBuf.data() + produced * channels_, 0,
                  (bufferFrames_ - produced) * channels_ * sizeof(float));
      BumpXrun();
    }
    ConvertFromFloat(floatBuf.data(), bufferFrames_, channels_, fmt_,
                     allocBuf_.data() + i * bufBytes);
    DWORD written = 0;
    WriteFile(pin_, allocBuf_.data() + i * bufBytes,
              static_cast<DWORD>(bufBytes), &written, &overlaps_[i]);
  }

  while (running_) {
    DWORD w = WaitForMultipleObjects(numBuffers_, overlapEvents_.data(),
                                     FALSE, 100);
    if (w == WAIT_TIMEOUT) {
      if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;
      continue;
    }
    if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + numBuffers_) {
      uint32_t idx = w - WAIT_OBJECT_0;
      DWORD written = 0;
      GetOverlappedResult(pin_, &overlaps_[idx], &written, FALSE);
      ResetEvent(overlapEvents_[idx]);

      uint32_t produced = dataCb_(floatBuf.data(), bufferFrames_);
      if (produced < bufferFrames_) {
        std::memset(floatBuf.data() + produced * channels_, 0,
                    (bufferFrames_ - produced) * channels_ * sizeof(float));
        BumpXrun();
      }
      ConvertFromFloat(floatBuf.data(), bufferFrames_, channels_, fmt_,
                       allocBuf_.data() + idx * bufBytes);
      WriteFile(pin_, allocBuf_.data() + idx * bufBytes,
                static_cast<DWORD>(bufBytes), &written, &overlaps_[idx]);
      framesProcessed_.fetch_add(bufferFrames_);
    }
  }

  EndMmcssThread(mmcssTask_); mmcssTask_ = nullptr;
  CoUninitialize();
}

void WdmEngine::CaptureLoop() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  mmcssTask_ = BeginMmcssThread(L"Pro Audio");

  size_t frameBytes = channels_ * BytesPerSample(fmt_);
  size_t bufBytes   = bufferFrames_ * frameBytes;
  std::vector<float> floatBuf(bufferFrames_ * channels_);

  // Queue all capture buffers.
  for (uint32_t i = 0; i < numBuffers_; ++i) {
    DWORD read = 0;
    ReadFile(pin_, allocBuf_.data() + i * bufBytes,
             static_cast<DWORD>(bufBytes), &read, &overlaps_[i]);
  }

  while (running_) {
    DWORD w = WaitForMultipleObjects(numBuffers_, overlapEvents_.data(),
                                     FALSE, 100);
    if (w == WAIT_TIMEOUT) {
      if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;
      continue;
    }
    if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + numBuffers_) {
      uint32_t idx = w - WAIT_OBJECT_0;
      DWORD bytesRead = 0;
      GetOverlappedResult(pin_, &overlaps_[idx], &bytesRead, FALSE);
      ResetEvent(overlapEvents_[idx]);

      uint32_t frames = static_cast<uint32_t>(bytesRead / frameBytes);
      if (frames > 0) {
        if (fmt_ == SampleFormat::F32) {
          dataCb_(reinterpret_cast<float*>(allocBuf_.data() + idx * bufBytes),
                  frames);
        } else {
          ConvertToFloat(allocBuf_.data() + idx * bufBytes, frames, channels_,
                         fmt_, floatBuf.data());
          dataCb_(floatBuf.data(), frames);
        }
        framesProcessed_.fetch_add(frames);
      }

      DWORD read = 0;
      ReadFile(pin_, allocBuf_.data() + idx * bufBytes,
               static_cast<DWORD>(bufBytes), &read, &overlaps_[idx]);
    }
  }

  EndMmcssThread(mmcssTask_); mmcssTask_ = nullptr;
  CoUninitialize();
}

}  // namespace nwa
