// mme_engine.cc - Implementation of the MME (winmm) backend.
//
// MME is the oldest Windows audio API but still works on every Windows version.
// Latency is high (typically 30-100ms) because winmm buffers are large, but
// it's a reliable fallback when WASAPI/ASIO are unavailable.
//
// mme_engine.h pulls in win_headers.h, which already includes windows.h,
// mmsystem.h, ks.h, ksmedia.h and the SPEAKER_* fallback macros in the
// correct order - no extra SDK includes needed here.

#include "mme_engine.h"

#include <algorithm>
#include <cstring>

namespace nwa {

namespace {

// Resolve a friendly device name into a uDeviceID for waveOutOpen/waveInOpen.
// Returns WAVE_MAPPER (-1) when id is empty.
UINT ResolveDeviceId(const std::string& id, bool output) {
  if (id.empty()) return WAVE_MAPPER;

  UINT n = output ? waveOutGetNumDevs() : waveInGetNumDevs();
  for (UINT i = 0; i < n; ++i) {
    WAVEOUTCAPSW oc{};
    WAVEINCAPSW  ic{};
    std::wstring name;
    if (output) {
      if (waveOutGetDevCapsW(i, &oc, sizeof(oc)) == MMSYSERR_NOERROR)
        name = oc.szPname;
    } else {
      if (waveInGetDevCapsW(i, &ic, sizeof(ic)) == MMSYSERR_NOERROR)
        name = ic.szPname;
    }
    if (!name.empty() && _stricmp(WideToUtf8(name.c_str()).c_str(),
                                  id.c_str()) == 0) {
      return i;
    }
  }
  // Fall back to numeric id if the string is parseable.
  int idx = atoi(id.c_str());
  if (idx >= 0 && static_cast<UINT>(idx) < n) return static_cast<UINT>(idx);
  return WAVE_MAPPER;
}

WORD BitsFor(SampleFormat f) {
  switch (f) {
    case SampleFormat::U8:     return 8;
    case SampleFormat::S16:    return 16;
    case SampleFormat::S24:    return 24;
    case SampleFormat::S24_32: return 32;
    case SampleFormat::S32:    return 32;
    case SampleFormat::F32:    return 32;
    default:                   return 16;
  }
}

bool BuildWaveFormat(SampleFormat f, uint32_t rate, uint16_t channels,
                     WAVEFORMATEXTENSIBLE* out) {
  WORD bits = BitsFor(f);
  out->Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
  out->Format.nChannels       = channels;
  out->Format.nSamplesPerSec  = rate;
  out->Format.wBitsPerSample  = bits;
  out->Format.nBlockAlign     = static_cast<WORD>(bits / 8 * channels);
  out->Format.nAvgBytesPerSec = rate * out->Format.nBlockAlign;
  out->Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  out->Samples.wValidBitsPerSample = bits;
  out->dwChannelMask = (channels >= 8) ? SPEAKER_7POINT1
                      : (channels >= 6) ? SPEAKER_5POINT1
                      : (channels >= 4) ? SPEAKER_QUAD
                      : (channels >= 2) ? SPEAKER_STEREO
                      : SPEAKER_MONO;
  out->SubFormat = (f == SampleFormat::F32 || f == SampleFormat::F64)
                     ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                     : KSDATAFORMAT_SUBTYPE_PCM;
  return true;
}

}  // namespace

MmeEngine::~MmeEngine() { Close(); }

Status MmeEngine::EnumerateDevices(StreamDirection dir,
                                   std::vector<DeviceInfo>* out) {
  if (!out) return Status::InvalidArgument;
  out->clear();

  if (dir == StreamDirection::Output) {
    UINT n = waveOutGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
      WAVEOUTCAPSW c{};
      if (waveOutGetDevCapsW(i, &c, sizeof(c)) != MMSYSERR_NOERROR) continue;
      DeviceInfo info;
      info.id            = std::to_string(i);
      info.name          = WideToUtf8(c.szPname);
      info.direction     = StreamDirection::Output;
      info.maxOutputChannels = c.wChannels;
      info.supportedSampleRates = {44100, 48000, 96000};
      info.supportedFormats     = {SampleFormat::U8, SampleFormat::S16,
                                   SampleFormat::S24, SampleFormat::S32,
                                   SampleFormat::F32};
      if (i == WAVE_MAPPER) info.isDefaultOutput = true;
      out->push_back(std::move(info));
    }
  } else {
    UINT n = waveInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
      WAVEINCAPSW c{};
      if (waveInGetDevCapsW(i, &c, sizeof(c)) != MMSYSERR_NOERROR) continue;
      DeviceInfo info;
      info.id            = std::to_string(i);
      info.name          = WideToUtf8(c.szPname);
      info.direction     = StreamDirection::Input;
      info.maxInputChannels = c.wChannels;
      info.supportedSampleRates = {44100, 48000, 96000};
      info.supportedFormats     = {SampleFormat::U8, SampleFormat::S16,
                                   SampleFormat::S24, SampleFormat::S32,
                                   SampleFormat::F32};
      if (i == WAVE_MAPPER) info.isDefaultInput = true;
      out->push_back(std::move(info));
    }
  }
  return Status::Ok;
}

Status MmeEngine::IsFormatSupported(const StreamConfig& cfg,
                                    SampleFormatInfo* nearest) {
  WAVEFORMATEXTENSIBLE wfx{};
  if (!BuildWaveFormat(cfg.format, cfg.sampleRate, cfg.channels, &wfx))
    return Status::FormatNotSupported;

  UINT devId = ResolveDeviceId(cfg.deviceId, cfg.direction == StreamDirection::Output);
  MMRESULT r;
  if (cfg.direction == StreamDirection::Output) {
    r = waveOutOpen(nullptr, devId,
                    reinterpret_cast<LPCWAVEFORMATEX>(&wfx), 0, 0,
                    WAVE_FORMAT_QUERY | WAVE_MAPPED);
  } else {
    r = waveInOpen(nullptr, devId,
                   reinterpret_cast<LPCWAVEFORMATEX>(&wfx), 0, 0,
                   WAVE_FORMAT_QUERY | WAVE_MAPPED);
  }
  if (nearest) {
    nearest->format     = cfg.format;
    nearest->sampleRate = cfg.sampleRate;
    nearest->channels   = cfg.channels;
  }
  return (r == MMSYSERR_NOERROR) ? Status::Ok
                                 : Status::FormatNotSupported;
}

Status MmeEngine::Open(const StreamConfig& cfg, DataCallback dataCb,
                       ErrorCallback errorCb) {
  if (opened_) return Status::AlreadyInitialized;
  cfg_      = cfg;
  fmt_      = cfg.format;
  channels_ = cfg.channels;
  rate_     = cfg.sampleRate;
  dataCb_   = std::move(dataCb);
  errorCb_  = std::move(errorCb);

  if (fmt_ == SampleFormat::F64 || fmt_ == SampleFormat::Unknown)
    return Status::FormatNotSupported;

  WAVEFORMATEXTENSIBLE wfx{};
  if (!BuildWaveFormat(fmt_, rate_, channels_, &wfx))
    return Status::FormatNotSupported;

  bufferFrames_ = cfg.bufferFrames > 0 ? cfg.bufferFrames : 1024;
  numBuffers_   = cfg.numBuffers   > 0 ? cfg.numBuffers   : 4;

  UINT devId = ResolveDeviceId(cfg.deviceId,
                               cfg.direction == StreamDirection::Output);

  if (cfg.direction == StreamDirection::Output) {
    MMRESULT r = waveOutOpen(&hOut_, devId,
                             reinterpret_cast<LPCWAVEFORMATEX>(&wfx),
                             reinterpret_cast<DWORD_PTR>(&MmeEngine::WaveOutProc),
                             reinterpret_cast<DWORD_PTR>(this),
                             CALLBACK_FUNCTION | WAVE_MAPPED);
    if (r != MMSYSERR_NOERROR) return Status::DeviceUnavailable;
  } else {
    MMRESULT r = waveInOpen(&hIn_, devId,
                            reinterpret_cast<LPCWAVEFORMATEX>(&wfx),
                            reinterpret_cast<DWORD_PTR>(&MmeEngine::WaveInProc),
                            reinterpret_cast<DWORD_PTR>(this),
                            CALLBACK_FUNCTION | WAVE_MAPPED);
    if (r != MMSYSERR_NOERROR) return Status::DeviceUnavailable;
  }

  size_t frameBytes = channels_ * BytesPerSample(fmt_);
  headers_.resize(numBuffers_);
  buffers_.resize(numBuffers_);
  freeFlags_.assign(numBuffers_, 0);
  for (uint32_t i = 0; i < numBuffers_; ++i) {
    buffers_[i].assign(bufferFrames_ * frameBytes, 0);
    headers_[i].lpData         = reinterpret_cast<LPSTR>(buffers_[i].data());
    headers_[i].dwBufferLength = static_cast<DWORD>(buffers_[i].size());
    headers_[i].dwFlags        = 0;
    if (cfg.direction == StreamDirection::Output) {
      waveOutPrepareHeader(hOut_, &headers_[i], sizeof(WAVEHDR));
    } else {
      waveInPrepareHeader(hIn_, &headers_[i], sizeof(WAVEHDR));
      waveInAddBuffer(hIn_, &headers_[i], sizeof(WAVEHDR));
    }
  }

  queueEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  stopEvent_  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!queueEvent_ || !stopEvent_) { Close(); return Status::BackendError; }

  opened_ = true;
  return Status::Ok;
}

Status MmeEngine::Start() {
  if (!opened_) return Status::NotInitialized;
  if (running_) return Status::AlreadyRunning;
  running_ = true;
  ResetEvent(stopEvent_);
  if (cfg_.direction == StreamDirection::Output) {
    // Prime all buffers.
    for (uint32_t i = 0; i < numBuffers_; ++i) {
      freeFlags_[i] = 1;
      waveOutWrite(hOut_, &headers_[i], sizeof(WAVEHDR));
    }
    thread_ = std::thread([this] { RenderThread(); });
  } else {
    waveInStart(hIn_);
    thread_ = std::thread([this] { CaptureThread(); });
  }
  return Status::Ok;
}

Status MmeEngine::Stop() {
  if (!running_) return Status::NotRunning;
  running_ = false;
  SetEvent(stopEvent_);
  if (hOut_) waveOutReset(hOut_);
  if (hIn_)  waveInReset(hIn_);
  if (thread_.joinable()) thread_.join();
  return Status::Ok;
}

Status MmeEngine::Close() {
  if (running_) Stop();
  for (uint32_t i = 0; i < headers_.size(); ++i) {
    if (hOut_ && (headers_[i].dwFlags & WHDR_PREPARED))
      waveOutUnprepareHeader(hOut_, &headers_[i], sizeof(WAVEHDR));
    if (hIn_  && (headers_[i].dwFlags & WHDR_PREPARED))
      waveInUnprepareHeader(hIn_, &headers_[i], sizeof(WAVEHDR));
  }
  if (hOut_) { waveOutClose(hOut_); hOut_ = nullptr; }
  if (hIn_)  { waveInClose(hIn_);  hIn_  = nullptr; }
  if (queueEvent_) { CloseHandle(queueEvent_); queueEvent_ = nullptr; }
  if (stopEvent_)  { CloseHandle(stopEvent_);  stopEvent_  = nullptr; }
  headers_.clear();
  buffers_.clear();
  freeFlags_.clear();
  opened_ = false;
  return Status::Ok;
}

Status MmeEngine::Latency(uint32_t* inputFrames, uint32_t* outputFrames) {
  if (outputFrames) *outputFrames = bufferFrames_ * numBuffers_;
  if (inputFrames)  *inputFrames  = bufferFrames_ * numBuffers_;
  return Status::Ok;
}

double MmeEngine::StreamTimeSeconds() {
  return static_cast<double>(framesConsumed_.load()) /
         static_cast<double>(rate_);
}

void CALLBACK MmeEngine::WaveOutProc(HWAVEOUT, UINT msg, DWORD_PTR inst,
                                     DWORD_PTR p1, DWORD_PTR) {
  auto* self = reinterpret_cast<MmeEngine*>(inst);
  if (!self) return;
  if (msg == WOM_DONE) {
    if (p1) self->OnOutDone(reinterpret_cast<WAVEHDR*>(p1));
  }
}

void CALLBACK MmeEngine::WaveInProc(HWAVEIN, UINT msg, DWORD_PTR inst,
                                    DWORD_PTR p1, DWORD_PTR) {
  auto* self = reinterpret_cast<MmeEngine*>(inst);
  if (!self) return;
  if (msg == WIM_DATA) {
    if (p1) self->OnInData(reinterpret_cast<WAVEHDR*>(p1));
  }
}

void MmeEngine::OnOutDone(WAVEHDR* hdr) {
  // Mark the buffer free and wake the render thread.
  for (uint32_t i = 0; i < headers_.size(); ++i) {
    if (&headers_[i] == hdr) {
      freeFlags_[i] = 0;
      break;
    }
  }
  SetEvent(queueEvent_);
}

void MmeEngine::OnInData(WAVEHDR* hdr) {
  // Hand off the captured data immediately on the MME thread.
  for (uint32_t i = 0; i < headers_.size(); ++i) {
    if (&headers_[i] == hdr) {
      freeFlags_[i] = 1;
      break;
    }
  }
  SetEvent(queueEvent_);
}

void MmeEngine::RenderThread() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  mmcssTask_ = BeginMmcssThread(L"Pro Audio");

  std::vector<float> floatBuf(bufferFrames_ * channels_);

  while (running_) {
    int freeIdx = -1;
    for (uint32_t i = 0; i < freeFlags_.size(); ++i) {
      if (freeFlags_[i] == 0) { freeIdx = static_cast<int>(i); break; }
    }
    if (freeIdx < 0) {
      HANDLE waits[2] = { stopEvent_, queueEvent_ };
      DWORD w = WaitForMultipleObjects(2, waits, FALSE, 100);
      if (w == WAIT_OBJECT_0) break;
      continue;
    }

    uint32_t produced = dataCb_(floatBuf.data(), bufferFrames_);
    if (produced < bufferFrames_) {
      std::memset(floatBuf.data() + produced * channels_, 0,
                  (bufferFrames_ - produced) * channels_ * sizeof(float));
      BumpXrun();
    }
    if (fmt_ == SampleFormat::F32) {
      std::memcpy(headers_[freeIdx].lpData, floatBuf.data(),
                  bufferFrames_ * channels_ * sizeof(float));
    } else {
      ConvertFromFloat(floatBuf.data(), bufferFrames_, channels_, fmt_,
                       headers_[freeIdx].lpData);
    }
    headers_[freeIdx].dwFlags &= ~WHDR_DONE;
    freeFlags_[freeIdx] = 1;
    waveOutWrite(hOut_, &headers_[freeIdx], sizeof(WAVEHDR));
    framesConsumed_.fetch_add(bufferFrames_);
  }

  EndMmcssThread(mmcssTask_); mmcssTask_ = nullptr;
  CoUninitialize();
}

void MmeEngine::CaptureThread() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  mmcssTask_ = BeginMmcssThread(L"Pro Audio");

  std::vector<float> floatBuf(bufferFrames_ * channels_);

  while (running_) {
    // Find any input buffer flagged "ready" (returned by MME).
    int readyIdx = -1;
    for (uint32_t i = 0; i < freeFlags_.size(); ++i) {
      if (freeFlags_[i] == 1 && (headers_[i].dwFlags & WHDR_DONE)) {
        readyIdx = static_cast<int>(i);
        break;
      }
    }
    if (readyIdx < 0) {
      HANDLE waits[2] = { stopEvent_, queueEvent_ };
      DWORD w = WaitForMultipleObjects(2, waits, FALSE, 100);
      if (w == WAIT_OBJECT_0) break;
      continue;
    }

    uint32_t frames = headers_[readyIdx].dwBytesRecorded /
                      (channels_ * BytesPerSample(fmt_));
    if (frames > 0) {
      if (fmt_ == SampleFormat::F32) {
        dataCb_(reinterpret_cast<float*>(headers_[readyIdx].lpData), frames);
      } else {
        ConvertToFloat(headers_[readyIdx].lpData, frames, channels_, fmt_,
                       floatBuf.data());
        dataCb_(floatBuf.data(), frames);
      }
      framesConsumed_.fetch_add(frames);
    }
    headers_[readyIdx].dwFlags &= ~WHDR_DONE;
    freeFlags_[readyIdx] = 0;
    waveInAddBuffer(hIn_, &headers_[readyIdx], sizeof(WAVEHDR));
  }

  EndMmcssThread(mmcssTask_); mmcssTask_ = nullptr;
  CoUninitialize();
}

}  // namespace nwa
