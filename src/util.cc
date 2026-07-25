// util.cc - Implementation of common helpers.
#include "util.h"

#include <avrt.h>
#include <mmreg.h>
#include <audioclient.h>   // AUDCLNT_E_* HRESULTs in HresultToStatus
// ksmedia.h hard-requires ks.h to be included first (#error otherwise).
#include <ks.h>
#include <ksmedia.h>

#include <cmath>
#include <cstdio>

#pragma comment(lib, "avrt.lib")

namespace nwa {

const char* StatusToString(Status s) {
  switch (s) {
    case Status::Ok:                  return "ok";
    case Status::InvalidArgument:     return "invalid argument";
    case Status::NotSupported:        return "not supported";
    case Status::DeviceNotFound:      return "device not found";
    case Status::DeviceUnavailable:   return "device unavailable";
    case Status::FormatNotSupported:  return "format not supported";
    case Status::AlreadyInitialized:  return "already initialized";
    case Status::NotInitialized:      return "not initialized";
    case Status::AlreadyRunning:      return "already running";
    case Status::NotRunning:          return "not running";
    case Status::Underrun:            return "underrun";
    case Status::Overrun:             return "overrun";
    case Status::BackendError:        return "backend error";
    case Status::OutOfMemory:         return "out of memory";
    case Status::TimedOut:            return "timed out";
    case Status::Interrupted:         return "interrupted";
    case Status::PermissionDenied:    return "permission denied";
    default:                          return "unknown error";
  }
}

size_t BytesPerSample(SampleFormat fmt) {
  switch (fmt) {
    case SampleFormat::U8:      return 1;
    case SampleFormat::S16:     return 2;
    case SampleFormat::S24:     return 3;
    case SampleFormat::S24_32:  return 4;
    case SampleFormat::S32:     return 4;
    case SampleFormat::F32:     return 4;
    case SampleFormat::F64:     return 8;
    default:                    return 0;
  }
}

Status HresultToStatus(HRESULT hr) {
  if (SUCCEEDED(hr)) return Status::Ok;

  // AUDCLNT_* error codes share FACILITY_AUDCLNT (0x889)
  switch (hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED:    return Status::DeviceUnavailable;
    case AUDCLNT_E_DEVICE_IN_USE:         return Status::DeviceUnavailable;
    case AUDCLNT_E_NOT_INITIALIZED:       return Status::NotInitialized;
    case AUDCLNT_E_ALREADY_INITIALIZED:   return Status::AlreadyInitialized;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:    return Status::FormatNotSupported;
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
                                           return Status::NotSupported;
    case AUDCLNT_E_BUFFER_ERROR:           return Status::BackendError;
    case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED:return Status::FormatNotSupported;
    case AUDCLNT_E_OUT_OF_ORDER:           return Status::BackendError;
    case AUDCLNT_E_NOT_STOPPED:            return Status::AlreadyRunning;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:    return Status::BackendError;
    case E_INVALIDARG:                     return Status::InvalidArgument;
    case E_OUTOFMEMORY:                    return Status::OutOfMemory;
    case E_NOTIMPL:                        return Status::NotSupported;
    case E_ACCESSDENIED:                   return Status::PermissionDenied;
    default:                               return Status::BackendError;
  }
}

std::string HresultToString(HRESULT hr) {
  wchar_t* wbuf = nullptr;
  DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                FORMAT_MESSAGE_IGNORE_INSERTS;
  DWORD len = FormatMessageW(flags, nullptr, hr,
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             reinterpret_cast<LPWSTR>(&wbuf), 0, nullptr);
  std::string out;
  if (len > 0 && wbuf) {
    out = WideToUtf8(wbuf, static_cast<int>(len));
    // Trim trailing whitespace/newlines.
    while (!out.empty() && (out.back() == '\r' || out.back() == '\n' ||
                            out.back() == ' ')) {
      out.pop_back();
    }
  } else {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(hr));
    out = buf;
  }
  if (wbuf) LocalFree(wbuf);
  return out;
}

std::string WideToUtf8(const wchar_t* w, int len) {
  if (!w) return std::string();
  if (len < 0) len = static_cast<int>(wcslen(w));
  if (len == 0) return std::string();
  int needed = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0,
                                   nullptr, nullptr);
  std::string out(needed, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, len, &out[0], needed, nullptr, nullptr);
  return out;
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                   static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(needed, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                      &out[0], needed);
  return out;
}

uint16_t SampleFormatToWaveFormatTag(SampleFormat f) {
  switch (f) {
    case SampleFormat::U8:
    case SampleFormat::S16:
    case SampleFormat::S24:
    case SampleFormat::S24_32:
    case SampleFormat::S32:    return WAVE_FORMAT_PCM;
    case SampleFormat::F32:
    case SampleFormat::F64:    return WAVE_FORMAT_IEEE_FLOAT;
    default:                   return WAVE_FORMAT_UNKNOWN;
  }
}

// ---------------------------------------------------------------------------
// PCM <-> float conversion
// ---------------------------------------------------------------------------
template <typename T>
static void CopyFloatGeneric(const T* src, uint32_t frames, uint16_t ch,
                             float* dst, float scale) {
  const uint32_t n = frames * ch;
  for (uint32_t i = 0; i < n; ++i) dst[i] = static_cast<float>(src[i]) * scale;
}

template <typename T>
static void CopyFloatToGeneric(const float* src, uint32_t frames, uint16_t ch,
                               T* dst, float scale) {
  const uint32_t n = frames * ch;
  for (uint32_t i = 0; i < n; ++i) {
    float v = src[i] * scale;
    if (v >  1.0f) v =  1.0f;
    if (v < -1.0f) v = -1.0f;
    dst[i] = static_cast<T>(v);
  }
}

void ConvertToFloat(const void* src, uint32_t frames, uint16_t channels,
                    SampleFormat srcFmt, float* dst) {
  const uint32_t n = frames * channels;
  switch (srcFmt) {
    case SampleFormat::F32:
      if (src != dst) std::memcpy(dst, src, n * sizeof(float));
      return;
    case SampleFormat::U8: {
      const uint8_t* s = reinterpret_cast<const uint8_t*>(src);
      for (uint32_t i = 0; i < n; ++i)
        dst[i] = (static_cast<int>(s[i]) - 128) * (1.0f / 128.0f);
      return;
    }
    case SampleFormat::S16:
      CopyFloatGeneric(reinterpret_cast<const int16_t*>(src), frames, channels,
                       dst, 1.0f / 32768.0f);
      return;
    case SampleFormat::S32:
      CopyFloatGeneric(reinterpret_cast<const int32_t*>(src), frames, channels,
                       dst, 1.0f / 2147483648.0f);
      return;
    case SampleFormat::S24_32:
      // 24-bit value left-aligned into int32 (WASAPI common case).
      CopyFloatGeneric(reinterpret_cast<const int32_t*>(src), frames, channels,
                       dst, 1.0f / 8388608.0f);
      return;
    case SampleFormat::S24: {
      const uint8_t* s = reinterpret_cast<const uint8_t*>(src);
      for (uint32_t i = 0; i < n; ++i) {
        int32_t v = (static_cast<int32_t>(s[i * 3 + 2]) << 16) |
                    (static_cast<int32_t>(s[i * 3 + 1]) << 8)  |
                     static_cast<int32_t>(s[i * 3]);
        if (v & 0x800000) v |= 0xFF000000;  // sign extend
        dst[i] = static_cast<float>(v) * (1.0f / 8388608.0f);
      }
      return;
    }
    case SampleFormat::F64: {
      const double* s = reinterpret_cast<const double*>(src);
      for (uint32_t i = 0; i < n; ++i)
        dst[i] = static_cast<float>(s[i]);
      return;
    }
    default:
      std::memset(dst, 0, n * sizeof(float));
      return;
  }
}

void ConvertFromFloat(const float* src, uint32_t frames, uint16_t channels,
                      SampleFormat dstFmt, void* dst) {
  const uint32_t n = frames * channels;
  switch (dstFmt) {
    case SampleFormat::F32:
      if (src != dst) std::memcpy(dst, src, n * sizeof(float));
      return;
    case SampleFormat::U8: {
      uint8_t* d = reinterpret_cast<uint8_t*>(dst);
      for (uint32_t i = 0; i < n; ++i) {
        int v = static_cast<int>(src[i] * 128.0f + 128.5f);
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        d[i] = static_cast<uint8_t>(v);
      }
      return;
    }
    case SampleFormat::S16:
      CopyFloatToGeneric(src, frames, channels,
                         reinterpret_cast<int16_t*>(dst), 32767.0f);
      return;
    case SampleFormat::S32:
      CopyFloatToGeneric(src, frames, channels,
                         reinterpret_cast<int32_t*>(dst), 2147483647.0f);
      return;
    case SampleFormat::S24_32:
      CopyFloatToGeneric(src, frames, channels,
                         reinterpret_cast<int32_t*>(dst), 8388607.0f);
      return;
    case SampleFormat::S24: {
      uint8_t* d = reinterpret_cast<uint8_t*>(dst);
      for (uint32_t i = 0; i < n; ++i) {
        int32_t v = static_cast<int32_t>(src[i] * 8388607.0f);
        if (v >  8388607) v =  8388607;
        if (v < -8388608) v = -8388608;
        d[i * 3]     = static_cast<uint8_t>(v & 0xFF);
        d[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        d[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
      }
      return;
    }
    case SampleFormat::F64: {
      double* d = reinterpret_cast<double*>(dst);
      for (uint32_t i = 0; i < n; ++i) d[i] = static_cast<double>(src[i]);
      return;
    }
    default:
      std::memset(dst, 0, n * BytesPerSample(dstFmt));
      return;
  }
}

HANDLE BeginMmcssThread(const wchar_t* taskName) {
  DWORD taskIndex = 0;
  HANDLE task = AvSetMmThreadCharacteristicsW(taskName, &taskIndex);
  return task;
}

void EndMmcssThread(HANDLE task) {
  if (task) AvRevertMmThreadCharacteristics(task);
}

void YieldSpin() {
  YieldProcessor();
  YieldProcessor();
  YieldProcessor();
  YieldProcessor();
}

}  // namespace nwa
