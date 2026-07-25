// asio_engine.cc - Implementation of the ASIO backend.
//
// The ASIO driver model is COM-like.  Drivers register their CLSID under
// HKLM\SOFTWARE\ASIO\<Name>.  Hosts enumerate these keys, then CoCreateInstance
// on the CLSID to obtain an IASIO interface pointer.  After Init/Start the
// driver calls our `bufferSwitch` callback on its own thread when buffers are
// ready to consume/produce.
//
// Because ASIO does not pass a user-data pointer to callbacks, only one ASIO
// stream can be active per process at a time.  This matches the design of most
// ASIO drivers (each driver model is itself exclusive to one client).

#include "asio_engine.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace nwa {

AsioEngine* AsioEngine::activeInstance_ = nullptr;

namespace {

const IID IID_IASIO_ =
    {0xA91DABA8, 0xC2AE, 0x11D3,
     {0xBD, 0xE7, 0x00, 0x10, 0x5A, 0x11, 0x06, 0xD9}};

// Walk HKLM\SOFTWARE\ASIO and collect (name -> CLSID).
bool EnumerateAsioDrivers(std::vector<std::pair<std::string, std::string>>* out) {
  out->clear();
  HKEY hAsio = nullptr;
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ,
                    &hAsio) != ERROR_SUCCESS) {
    // 32-bit view fallback.
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\ASIO", 0,
                      KEY_READ, &hAsio) != ERROR_SUCCESS) {
      return false;
    }
  }
  wchar_t name[256];
  DWORD index = 0;
  while (true) {
    DWORD nameLen = 256;
    LONG r = RegEnumKeyW(hAsio, index, name, nameLen);
    if (r != ERROR_SUCCESS) break;
    ++index;

    HKEY hDriver = nullptr;
    if (RegOpenKeyExW(hAsio, name, 0, KEY_READ, &hDriver) != ERROR_SUCCESS)
      continue;
    wchar_t clsidBuf[64] = {0};
    DWORD bufLen = sizeof(clsidBuf);
    DWORD type = 0;
    if (RegQueryValueExW(hDriver, L"CLSID", nullptr, &type,
                         reinterpret_cast<LPBYTE>(clsidBuf), &bufLen) == ERROR_SUCCESS) {
      out->emplace_back(WideToUtf8(name), WideToUtf8(clsidBuf));
    }
    RegCloseKey(hDriver);
  }
  RegCloseKey(hAsio);
  return true;
}

// Convert an ASIO sample type into our SampleFormat.  ASIO exposes many types;
// we normalize to the most useful ones, otherwise return Unknown.
SampleFormat AsioTypeToSampleFormat(ASIOSampleType t) {
  switch (t) {
    case ASIOST_Int16LSB:    return SampleFormat::S16;
    case ASIOST_Int24LSB:    return SampleFormat::S24;
    case ASIOST_Int32LSB:    return SampleFormat::S32;
    case ASIOST_Int32LSB16:
    case ASIOST_Int32LSB18:
    case ASIOST_Int32LSB20:
    case ASIOST_Int32LSB24:  return SampleFormat::S24_32;
    case ASIOST_Float32LSB:  return SampleFormat::F32;
    case ASIOST_Float64LSB:  return SampleFormat::F64;
    default:                 return SampleFormat::Unknown;
  }
}

// Generic runtime conversion - slower but covers all types.
void ConvertChannelRuntime(const void* src, ASIOSampleType type,
                           uint32_t frames, uint16_t channels,
                           uint16_t ch, float* dst) {
  switch (type) {
    case ASIOST_Int16LSB: {
      const int16_t* s = static_cast<const int16_t*>(src);
      for (uint32_t i = 0; i < frames; ++i)
        dst[i * channels + ch] = static_cast<float>(s[i]) * (1.0f / 32768.0f);
      return;
    }
    case ASIOST_Int24LSB: {
      const uint8_t* s = static_cast<const uint8_t*>(src);
      for (uint32_t i = 0; i < frames; ++i) {
        int32_t v = (static_cast<int32_t>(s[i * 3]) << 8) |
                    (static_cast<int32_t>(s[i * 3 + 1]) << 16) |
                    (static_cast<int32_t>(s[i * 3 + 2]) << 24);
        v >>= 8;  // sign-extend
        dst[i * channels + ch] = static_cast<float>(v) * (1.0f / 8388608.0f);
      }
      return;
    }
    case ASIOST_Int32LSB: {
      const int32_t* s = static_cast<const int32_t*>(src);
      for (uint32_t i = 0; i < frames; ++i)
        dst[i * channels + ch] = static_cast<float>(s[i]) * (1.0f / 2147483648.0f);
      return;
    }
    case ASIOST_Int32LSB16:
    case ASIOST_Int32LSB18:
    case ASIOST_Int32LSB20:
    case ASIOST_Int32LSB24: {
      // 24-bit data left-aligned in int32.
      const int32_t* s = static_cast<const int32_t*>(src);
      for (uint32_t i = 0; i < frames; ++i)
        dst[i * channels + ch] = static_cast<float>(s[i]) * (1.0f / 8388608.0f);
      return;
    }
    case ASIOST_Float32LSB: {
      const float* s = static_cast<const float*>(src);
      for (uint32_t i = 0; i < frames; ++i)
        dst[i * channels + ch] = s[i];
      return;
    }
    case ASIOST_Float64LSB: {
      const double* s = static_cast<const double*>(src);
      for (uint32_t i = 0; i < frames; ++i)
        dst[i * channels + ch] = static_cast<float>(s[i]);
      return;
    }
    default:
      // Unknown - zero the channel.
      for (uint32_t i = 0; i < frames; ++i) dst[i * channels + ch] = 0.0f;
      return;
  }
}

// Convert one channel of an interleaved float buffer into an ASIO planar
// buffer in device format.
void ConvertChannelFromFloat(const float* src, uint16_t channels, uint16_t ch,
                             uint32_t frames, ASIOSampleType type, void* dst) {
  switch (type) {
    case ASIOST_Int16LSB: {
      int16_t* d = static_cast<int16_t*>(dst);
      for (uint32_t i = 0; i < frames; ++i) {
        float v = src[i * channels + ch] * 32767.0f;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        d[i] = static_cast<int16_t>(v);
      }
      return;
    }
    case ASIOST_Int24LSB: {
      uint8_t* d = static_cast<uint8_t*>(dst);
      for (uint32_t i = 0; i < frames; ++i) {
        int32_t v = static_cast<int32_t>(src[i * channels + ch] * 8388607.0f);
        if (v > 8388607)  v = 8388607;
        if (v < -8388608) v = -8388608;
        d[i * 3]     = static_cast<uint8_t>(v & 0xFF);
        d[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        d[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
      }
      return;
    }
    case ASIOST_Int32LSB: {
      int32_t* d = static_cast<int32_t*>(dst);
      for (uint32_t i = 0; i < frames; ++i) {
        float v = src[i * channels + ch] * 2147483647.0f;
        if (v > 2147483647.0f)  v = 2147483647.0f;
        if (v < -2147483648.0f) v = -2147483648.0f;
        d[i] = static_cast<int32_t>(v);
      }
      return;
    }
    case ASIOST_Int32LSB16:
    case ASIOST_Int32LSB18:
    case ASIOST_Int32LSB20:
    case ASIOST_Int32LSB24: {
      int32_t* d = static_cast<int32_t*>(dst);
      for (uint32_t i = 0; i < frames; ++i) {
        int32_t v = static_cast<int32_t>(src[i * channels + ch] * 8388607.0f);
        if (v > 8388607)  v = 8388607;
        if (v < -8388608) v = -8388608;
        d[i] = v << 8;
      }
      return;
    }
    case ASIOST_Float32LSB: {
      float* d = static_cast<float*>(dst);
      for (uint32_t i = 0; i < frames; ++i) d[i] = src[i * channels + ch];
      return;
    }
    case ASIOST_Float64LSB: {
      double* d = static_cast<double*>(dst);
      for (uint32_t i = 0; i < frames; ++i) d[i] = static_cast<double>(src[i * channels + ch]);
      return;
    }
    default:
      std::memset(dst, 0, frames * 4);   // best effort
      return;
  }
}

}  // namespace

AsioEngine::~AsioEngine() { Close(); }

bool AsioEngine::FindDriverClsid(const std::string& id,
                                 std::wstring* clsidWide,
                                 std::string* realName) {
  std::vector<std::pair<std::string, std::string>> drivers;
  if (!EnumerateAsioDrivers(&drivers) || drivers.empty()) return false;

  // Default = first registered driver.
  const std::pair<std::string, std::string>* picked = nullptr;
  if (id.empty()) {
    picked = &drivers.front();
  } else {
    for (const auto& d : drivers) {
      if (_stricmp(d.first.c_str(), id.c_str()) == 0) {
        picked = &d;
        break;
      }
    }
    if (!picked) picked = &drivers.front();
  }
  *realName  = picked->first;
  *clsidWide = Utf8ToWide(picked->second);
  return true;
}

Status AsioEngine::EnumerateDevices(StreamDirection dir,
                                    std::vector<DeviceInfo>* out) {
  if (!out) return Status::InvalidArgument;
  out->clear();

  std::vector<std::pair<std::string, std::string>> drivers;
  if (!EnumerateAsioDrivers(&drivers)) return Status::Ok;

  for (const auto& d : drivers) {
    DeviceInfo info;
    info.id        = d.first;          // use driver name as id
    info.name      = d.first;
    info.adapter   = "ASIO";
    info.direction = dir;
    info.maxInputChannels  = 64;
    info.maxOutputChannels = 64;
    info.supportedSampleRates = {44100, 48000, 88200, 96000, 176400, 192000};
    info.supportedFormats     = {SampleFormat::S16, SampleFormat::S24,
                                 SampleFormat::S32, SampleFormat::F32};
    out->push_back(std::move(info));
  }
  return Status::Ok;
}

Status AsioEngine::IsFormatSupported(const StreamConfig& cfg,
                                     SampleFormatInfo* nearest) {
  if (nearest) {
    nearest->format     = cfg.format;
    nearest->sampleRate = cfg.sampleRate;
    nearest->channels   = cfg.channels;
  }
  // Real probing requires instantiating the driver.  Optimistically report OK.
  return Status::Ok;
}

Status AsioEngine::Open(const StreamConfig& cfg, DataCallback dataCb,
                        ErrorCallback errorCb) {
  if (opened_) return Status::AlreadyInitialized;
  if (activeInstance_) return Status::NotSupported;  // only one ASIO stream

  cfg_      = cfg;
  fmt_      = cfg.format;
  channels_ = cfg.channels;
  rate_     = cfg.sampleRate;
  dataCb_   = std::move(dataCb);
  errorCb_  = std::move(errorCb);

  // Initialize COM on this thread.
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  comInit_ = SUCCEEDED(hr);
  if (hr != RPC_E_CHANGED_MODE && FAILED(hr)) return Status::BackendError;

  std::wstring clsidWide;
  std::string  name;
  if (!FindDriverClsid(cfg.deviceId, &clsidWide, &name))
    return Status::DeviceNotFound;

  CLSID clsid;
  hr = CLSIDFromString(clsidWide.c_str(), &clsid);
  if (FAILED(hr)) return Status::DeviceNotFound;

  hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_IASIO_,
                        reinterpret_cast<void**>(&asio_));
  if (FAILED(hr) || !asio_) return Status::DeviceUnavailable;

  if (!asio_->Init(nullptr)) {
    asio_->Release();
    asio_ = nullptr;
    return Status::BackendError;
  }

  if (asio_->GetChannels(&inputChannels_, &outputChannels_) != ASE_OK) {
    Close(); return Status::BackendError;
  }
  if (cfg.direction == StreamDirection::Output && outputChannels_ < static_cast<long>(channels_)) {
    Close(); return Status::FormatNotSupported;
  }
  if (cfg.direction == StreamDirection::Input && inputChannels_ < static_cast<long>(channels_)) {
    Close(); return Status::FormatNotSupported;
  }

  // Try to set the requested sample rate.
  ASIOSampleRate cur = 0;
  if (asio_->GetSampleRate(&cur) == ASE_OK) {
    if (static_cast<uint32_t>(cur) != rate_) {
      if (asio_->CanSampleRate(static_cast<ASIOSampleRate>(rate_)) == ASE_OK) {
        if (asio_->SetSampleRate(static_cast<ASIOSampleRate>(rate_)) != ASE_OK) {
          Close(); return Status::FormatNotSupported;
        }
      } else {
        // Driver can't do this rate - use whatever it's at.
        rate_ = static_cast<uint32_t>(cur);
      }
    }
  }

  long minSize = 0, maxSize = 0, prefSize = 0, gran = 0;
  if (asio_->GetBufferSize(&minSize, &maxSize, &prefSize, &gran) != ASE_OK) {
    Close(); return Status::BackendError;
  }
  bufferFrames_ = (cfg.bufferFrames > 0) ? cfg.bufferFrames : prefSize;
  // Snap to granularity if necessary.
  if (gran > 0 && bufferFrames_ < minSize) bufferFrames_ = minSize;
  if (gran > 0 && bufferFrames_ > maxSize) bufferFrames_ = maxSize;

  // Build the buffer info array.  We use the first N channels of the
  // requested direction.
  bool render = (cfg.direction == StreamDirection::Output);
  long activeChannels = static_cast<long>(channels_);
  buffers_.resize(activeChannels);
  for (long i = 0; i < activeChannels; ++i) {
    buffers_[i].isInput    = render ? 0 : 1;
    buffers_[i].channelNum = i;
    buffers_[i].buffers[0] = nullptr;
    buffers_[i].buffers[1] = nullptr;
  }

  callbacks_.bufferSwitch           = &AsioEngine::CallbackBufferSwitch;
  callbacks_.sampleRateDidChange    = &AsioEngine::CallbackSampleRateChanged;
  callbacks_.asioMessage            = &AsioEngine::CallbackAsioMessage;
  callbacks_.bufferSwitchTimeInfo   = &AsioEngine::CallbackBufferSwitchTimeInfo;

  if (asio_->CreateBuffers(buffers_.data(), activeChannels,
                           bufferFrames_, &callbacks_) != ASE_OK) {
    Close(); return Status::FormatNotSupported;
  }

  activeInstance_ = this;
  opened_ = true;
  return Status::Ok;
}

Status AsioEngine::Start() {
  if (!opened_) return Status::NotInitialized;
  if (running_) return Status::AlreadyRunning;
  mmcssTask_ = BeginMmcssThread(L"Pro Audio");
  if (asio_->Start() != ASE_OK) {
    EndMmcssThread(mmcssTask_); mmcssTask_ = nullptr;
    return Status::BackendError;
  }
  running_ = true;
  return Status::Ok;
}

Status AsioEngine::Stop() {
  if (!running_) return Status::NotRunning;
  running_ = false;
  if (asio_) asio_->Stop();
  EndMmcssThread(mmcssTask_); mmcssTask_ = nullptr;
  return Status::Ok;
}

Status AsioEngine::Close() {
  if (running_) Stop();
  if (asio_) {
    asio_->DisposeBuffers();
    asio_->Release();
    asio_ = nullptr;
  }
  if (activeInstance_ == this) activeInstance_ = nullptr;
  buffers_.clear();
  opened_ = false;
  if (comInit_) { CoUninitialize(); comInit_ = false; }
  return Status::Ok;
}

Status AsioEngine::Latency(uint32_t* inputFrames, uint32_t* outputFrames) {
  if (!opened_) return Status::NotInitialized;
  long in = 0, out = 0;
  if (asio_ && asio_->GetLatencies(&in, &out) == ASE_OK) {
    if (inputFrames)  *inputFrames  = in + bufferFrames_;
    if (outputFrames) *outputFrames = out + bufferFrames_;
    return Status::Ok;
  }
  return Status::NotSupported;
}

double AsioEngine::StreamTimeSeconds() {
  return static_cast<double>(framesProcessed_.load()) /
         static_cast<double>(rate_);
}

// ---------------------------------------------------------------------------
// Static callbacks (invoked by the ASIO driver on its own thread).
// ---------------------------------------------------------------------------

void AsioEngine::CallbackBufferSwitch(long doubleBufferIndex,
                                      ASIOBool directProcess) {
  if (activeInstance_) activeInstance_->OnBufferSwitch(doubleBufferIndex);
}

void AsioEngine::CallbackSampleRateChanged(ASIOSampleRate sRate) {
  if (activeInstance_ && activeInstance_->errorCb_) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "ASIO sample rate changed to %.0f Hz", sRate);
    activeInstance_->errorCb_(Status::BackendError, buf);
  }
}

ASIOTime* AsioEngine::CallbackBufferSwitchTimeInfo(ASIOTime* time,
                                                   long doubleBufferIndex,
                                                   ASIOBool directProcess) {
  CallbackBufferSwitch(doubleBufferIndex, directProcess);
  return time;
}

long AsioEngine::CallbackAsioMessage(long selector, long value, void*,
                                     double*) {
  // Selector supported?
  if (selector == kAsioSelectorSupported) {
    if (value == kAsioResetRequest        ||
        value == kAsioResyncRequest       ||
        value == kAsioLatenciesChanged    ||
        value == kAsioSupportsTimeInfo    ||
        value == kAsioSupportsTimeCode    ||
        value == kAsioEngineVersion       ||
        value == kAsioBufferSizeChange) {
      return 1L;
    }
    return 0L;
  }
  if (selector == kAsioEngineVersion)   return 2L;
  if (selector == kAsioResetRequest)    return 1L;
  if (selector == kAsioResyncRequest)   return 1L;
  if (selector == kAsioLatenciesChanged) return 1L;
  if (selector == kAsioBufferSizeChange) return 1L;
  return 0L;
}

void AsioEngine::OnBufferSwitch(long doubleBufferIndex) {
  if (!running_ || !dataCb_) return;

  // Step 1: gather per-channel sample types via GetChannelInfo.  This is the
  // slow path; in a production-quality implementation we'd cache these on
  // CreateBuffers.
  std::vector<ASIOSampleType> types(channels_);
  for (uint16_t i = 0; i < channels_; ++i) {
    ASIOChannelInfo ci{};
    ci.channel = i;
    ci.isInput = (cfg_.direction == StreamDirection::Input) ? 1 : 0;
    if (asio_->GetChannelInfo(&ci) == ASE_OK) types[i] = ci.type;
    else types[i] = ASIOST_Int32LSB;
  }

  std::vector<float> floatBuf(bufferFrames_ * channels_);

  if (cfg_.direction == StreamDirection::Input) {
    // Convert planar input buffers -> interleaved float, then deliver.
    for (uint16_t ch = 0; ch < channels_; ++ch) {
      void* src = buffers_[ch].buffers[doubleBufferIndex];
      ConvertChannelRuntime(src, types[ch], bufferFrames_, channels_, ch,
                            floatBuf.data());
    }
    dataCb_(floatBuf.data(), bufferFrames_);
  } else {
    // Ask user for float audio, then deinterleave into each planar output
    // channel.
    uint32_t produced = dataCb_(floatBuf.data(), bufferFrames_);
    if (produced < bufferFrames_) {
      std::memset(floatBuf.data() + produced * channels_, 0,
                  (bufferFrames_ - produced) * channels_ * sizeof(float));
      BumpXrun();
    }
    for (uint16_t ch = 0; ch < channels_; ++ch) {
      void* dst = buffers_[ch].buffers[doubleBufferIndex];
      ConvertChannelFromFloat(floatBuf.data(), channels_, ch, bufferFrames_,
                              types[ch], dst);
    }
  }
  framesProcessed_.fetch_add(bufferFrames_);
}

}  // namespace nwa
