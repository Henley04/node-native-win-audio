// audiograph_engine.cc - Stub implementation of the AudioGraph (WinRT) backend.
//
// The full C++/WinRT implementation of AudioGraph requires substantial build
// infrastructure (cppwinrt.exe code generation, windowsapp.lib linkage, RoInitialize
// apartment setup, async coroutine plumbing) that is fragile to wire up inside a
// node-gyp build.  Because the other five backends (WASAPI shared/exclusive, MME,
// WDM-KS, ASIO) already cover the latency-sensitive use cases AudioGraph would
// serve, this file ships a stub that reports the backend as unsupported.
//
// The JS layer surfaces this as `backend: 'audiograph'` returning
// `Status::NotSupported` from every operation, so callers can detect the
// condition and fall back to WASAPI (which is what AudioGraph uses internally
// anyway).

#include "audiograph_engine.h"

namespace nwa {

AudioGraphEngine::~AudioGraphEngine() { Close(); }

Status AudioGraphEngine::EnumerateDevices(StreamDirection /*dir*/,
                                          std::vector<DeviceInfo>* /*out*/) {
  return Status::NotSupported;
}

Status AudioGraphEngine::IsFormatSupported(const StreamConfig& /*cfg*/,
                                           SampleFormatInfo* /*nearest*/) {
  return Status::NotSupported;
}

Status AudioGraphEngine::Open(const StreamConfig& /*cfg*/,
                              DataCallback /*dataCb*/,
                              ErrorCallback /*errorCb*/) {
  return Status::NotSupported;
}

Status AudioGraphEngine::Start() { return Status::NotSupported; }
Status AudioGraphEngine::Stop()  { return Status::NotSupported; }
Status AudioGraphEngine::Close() { return Status::Ok; }

Status AudioGraphEngine::Latency(uint32_t* /*inputFrames*/,
                                 uint32_t* /*outputFrames*/) {
  return Status::NotSupported;
}

double AudioGraphEngine::StreamTimeSeconds() { return -1.0; }

}  // namespace nwa
