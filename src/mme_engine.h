// mme_engine.h - MME backend (winmm waveIn/waveOut).
#ifndef NODE_WIN_AUDIO_MME_ENGINE_H_
#define NODE_WIN_AUDIO_MME_ENGINE_H_

// mmsystem.h transitively pulls in mmsyscom.h, which references MMVERSION /
// HDRVR / FAR / DRVCALLBACK types that are only defined after windows.h has
// been processed.  Including mmsystem.h before windows.h therefore produces
// a flood of C4430/C3646/C2146 errors from inside the SDK header itself.
// NOMINMAX is asserted locally because util.h is included further down and
// we cannot rely on translation units seeing it first.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <atomic>
#include <thread>
#include <vector>

#include "audio_engine.h"
#include "util.h"

namespace nwa {

class MmeEngine : public AudioEngine {
 public:
  MmeEngine() = default;
  ~MmeEngine() override;

  Backend backend() const override { return Backend::Mme; }
  const char* Name() const override { return "MME (winmm)"; }

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
  // Static callback trampoline dispatched by winmm.
  static void CALLBACK WaveOutProc(HWAVEOUT h, UINT msg, DWORD_PTR inst,
                                   DWORD_PTR p1, DWORD_PTR p2);
  static void CALLBACK WaveInProc(HWAVEIN h, UINT msg, DWORD_PTR inst,
                                  DWORD_PTR p1, DWORD_PTR p2);
  void OnOutDone(WAVEHDR* hdr);
  void OnInData(WAVEHDR* hdr);

  // Worker thread: pulls free buffers and pumps the user callback.
  void RenderThread();
  void CaptureThread();

  bool opened_  = false;
  bool running_ = false;

  HWAVEOUT hOut_ = nullptr;
  HWAVEIN  hIn_  = nullptr;

  StreamConfig  cfg_;
  SampleFormat  fmt_       = SampleFormat::S16;
  uint16_t      channels_  = 2;
  uint32_t      rate_      = 48000;
  uint32_t      bufferFrames_ = 0;
  uint32_t      numBuffers_   = 4;
  DataCallback  dataCb_;
  ErrorCallback errorCb_;

  std::vector<WAVEHDR>  headers_;
  std::vector<std::vector<uint8_t>> buffers_;   // backing storage
  std::vector<uint8_t>  freeFlags_;             // 1 = buffer is queued out

  std::thread    thread_;
  HANDLE         mmcssTask_ = nullptr;
  HANDLE         queueEvent_ = nullptr;          // signaled when buffer freed
  HANDLE         stopEvent_  = nullptr;

  std::atomic<uint64_t> framesConsumed_{0};
};

}  // namespace nwa

#endif  // NODE_WIN_AUDIO_MME_ENGINE_H_
