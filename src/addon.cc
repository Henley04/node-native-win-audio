// addon.cc - N-API bindings that expose the native audio engines to JS.
//
// Design
// ------
// Each audio stream is wrapped in a `StreamWrap` Napi::ObjectWrap.  The wrap
// owns:
//   * A concrete AudioEngine implementation (WASAPI / MME / WDM-KS / ASIO /
//     AudioGraph) selected at construction time.
//   * A thread-safe ring buffer for the audio data path.
//   * A Napi::ThreadSafeFunction used to deliver input chunks back to JS.
//
// Two streaming modes are supported:
//
//   Output (render): JS calls `stream.write(Float32Array)` to push interleaved
//     float samples into the ring buffer.  The native audio thread pulls from
//     the ring buffer; on underrun, silence is emitted and the xrun counter is
//     incremented.  JS can query `bufferedFrames` and top up before it drains.
//
//   Input (capture): The native audio thread accumulates captured samples and,
//     when a full chunk is ready, hands a Float32Array to JS via the
//     `on('data', cb)` callback (delivered through a ThreadSafeFunction).  JS
//     does not need to poll.
//
// This avoids blocking the high-priority audio thread on JS execution, which
// is the only practical way to keep GC pauses from causing dropouts.

// win_headers.h MUST be included before <napi.h>: node-addon-api's headers
// transitively include windows.h, and we need to control that inclusion
// (NOMINMAX set, WIN32_LEAN_AND_MEAN NOT set, mmreg.h / ks.h / ksmedia.h /
// mmsystem.h pulled in the correct order).  Including win_headers.h first
// makes napi.h's later windows.h include a no-op via header guards.
#include "win_headers.h"

#include <napi.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "audio_engine.h"
#include "asio_engine.h"
#include "audiograph_engine.h"
#include "mme_engine.h"
#include "util.h"
#include "wasapi_engine.h"
#include "wdm_engine.h"

namespace {

// ---------------------------------------------------------------------------
// Backend selection
// ---------------------------------------------------------------------------
std::unique_ptr<nwa::AudioEngine> CreateEngine(nwa::Backend b) {
  switch (b) {
    case nwa::Backend::Wasapi:          return std::make_unique<nwa::WasapiEngine>(false);
    case nwa::Backend::WasapiExclusive: return std::make_unique<nwa::WasapiEngine>(true);
    case nwa::Backend::Mme:             return std::make_unique<nwa::MmeEngine>();
    case nwa::Backend::Wdm:             return std::make_unique<nwa::WdmEngine>();
    case nwa::Backend::Asio:            return std::make_unique<nwa::AsioEngine>();
    case nwa::Backend::AudioGraph:      return std::make_unique<nwa::AudioGraphEngine>();
    default:                            return nullptr;
  }
}

nwa::Backend ParseBackend(const std::string& s) {
  if (s == "wasapi" || s == "WASAPI")           return nwa::Backend::Wasapi;
  if (s == "wasapi-exclusive" || s == "WASAPI-EXCLUSIVE")
                                                return nwa::Backend::WasapiExclusive;
  if (s == "asio"  || s == "ASIO")              return nwa::Backend::Asio;
  if (s == "mme"   || s == "MME")               return nwa::Backend::Mme;
  if (s == "wdm"   || s == "WDM" || s == "wdm-ks")
                                                return nwa::Backend::Wdm;
  if (s == "audiograph" || s == "AudioGraph")   return nwa::Backend::AudioGraph;
  return nwa::Backend::Wasapi;
}

nwa::SampleFormat ParseFormat(const std::string& s) {
  if (s == "u8")     return nwa::SampleFormat::U8;
  if (s == "s16")    return nwa::SampleFormat::S16;
  if (s == "s24")    return nwa::SampleFormat::S24;
  if (s == "s24-32" || s == "s24_32") return nwa::SampleFormat::S24_32;
  if (s == "s32")    return nwa::SampleFormat::S32;
  if (s == "f32")    return nwa::SampleFormat::F32;
  if (s == "f64")    return nwa::SampleFormat::F64;
  return nwa::SampleFormat::F32;
}

const char* FormatToString(nwa::SampleFormat f) {
  switch (f) {
    case nwa::SampleFormat::U8:     return "u8";
    case nwa::SampleFormat::S16:    return "s16";
    case nwa::SampleFormat::S24:    return "s24";
    case nwa::SampleFormat::S24_32: return "s24-32";
    case nwa::SampleFormat::S32:    return "s32";
    case nwa::SampleFormat::F32:    return "f32";
    case nwa::SampleFormat::F64:    return "f64";
    default:                        return "unknown";
  }
}

// ---------------------------------------------------------------------------
// Stream wrap
// ---------------------------------------------------------------------------
class StreamWrap : public Napi::ObjectWrap<StreamWrap> {
 public:
  static Napi::Object Init(Napi::Env env, Napi::Object exports) {
    Napi::Function func = DefineClass(env, "Stream", {
      InstanceMethod("start",           &StreamWrap::Start),
      InstanceMethod("stop",            &StreamWrap::Stop),
      InstanceMethod("close",           &StreamWrap::Close),
      InstanceMethod("write",           &StreamWrap::Write),
      InstanceMethod("read",            &StreamWrap::Read),
      InstanceMethod("flush",           &StreamWrap::Flush),
      InstanceMethod("latency",         &StreamWrap::Latency),
      InstanceMethod("xruns",           &StreamWrap::Xruns),
      InstanceMethod("streamTime",      &StreamWrap::StreamTime),
      InstanceMethod("bufferedFrames",  &StreamWrap::BufferedFrames),
      InstanceMethod("on",              &StreamWrap::On),
      InstanceAccessor("backend",       &StreamWrap::GetBackend, nullptr),
      InstanceAccessor("direction",     &StreamWrap::GetDirection, nullptr),
      InstanceAccessor("sampleRate",    &StreamWrap::GetSampleRate, nullptr),
      InstanceAccessor("channels",      &StreamWrap::GetChannels, nullptr),
      InstanceAccessor("format",        &StreamWrap::GetFormat, nullptr),
      InstanceAccessor("bufferFrames",  &StreamWrap::GetBufferFrames, nullptr),
    });
    Napi::FunctionReference* ctor = new Napi::FunctionReference();
    *ctor = Napi::Persistent(func);
    env.SetInstanceData(ctor);
    exports.Set("Stream", func);
    return exports;
  }

  explicit StreamWrap(const Napi::CallbackInfo& info)
      : Napi::ObjectWrap<StreamWrap>(info) {
    Napi::Env env = info.Env();
    if (info.Length() < 1 || !info[0].IsObject()) {
      Napi::TypeError::New(env, "Expected config object").ThrowAsJavaScriptException();
      return;
    }
    Napi::Object cfg = info[0].As<Napi::Object>();

    nwa::StreamConfig sc;
    std::string backendStr = cfg.Has("backend")
        ? cfg.Get("backend").As<Napi::String>().Utf8Value() : "wasapi";
    backend_ = ParseBackend(backendStr);
    sc.direction = (cfg.Has("direction") &&
                    cfg.Get("direction").As<Napi::String>().Utf8Value() == "input")
                       ? nwa::StreamDirection::Input
                       : nwa::StreamDirection::Output;
    if (cfg.Has("deviceId"))
      sc.deviceId = cfg.Get("deviceId").As<Napi::String>().Utf8Value();
    if (cfg.Has("sampleRate"))
      sc.sampleRate = cfg.Get("sampleRate").As<Napi::Number>().Uint32Value();
    if (cfg.Has("channels"))
      sc.channels = static_cast<uint16_t>(cfg.Get("channels").As<Napi::Number>().Uint32Value());
    if (cfg.Has("format"))
      sc.format = ParseFormat(cfg.Get("format").As<Napi::String>().Utf8Value());
    else
      sc.format = nwa::SampleFormat::F32;
    if (cfg.Has("bufferFrames"))
      sc.bufferFrames = cfg.Get("bufferFrames").As<Napi::Number>().Uint32Value();
    if (cfg.Has("numBuffers"))
      sc.numBuffers = cfg.Get("numBuffers").As<Napi::Number>().Uint32Value();
    if (cfg.Has("exclusive"))
      sc.exclusive = cfg.Get("exclusive").As<Napi::Boolean>().Value();
    if (cfg.Has("eventDriven"))
      sc.eventDriven = cfg.Get("eventDriven").As<Napi::Boolean>().Value();

    engine_ = CreateEngine(backend_);
    if (!engine_) {
      Napi::Error::New(env, "Unsupported backend").ThrowAsJavaScriptException();
      return;
    }

    // Ring buffer (size: ~1 second of audio).
    uint32_t ringFrames = sc.sampleRate;
    ring_.resize(ringFrames * sc.channels, 0.0f);
    ringMask_ = ring_.size() - 1;
    // Force power-of-two ring size.
    size_t pow2 = 1;
    while (pow2 < ring_.size()) pow2 <<= 1;
    ring_.resize(pow2, 0.0f);
    ringMask_ = pow2 - 1;

    // The native callback writes input chunks into the ring buffer and emits
    // 'data' events to JS.  For output it reads from the ring buffer.
    direction_ = sc.direction;
    channels_  = sc.channels;
    sampleRate_ = sc.sampleRate;
    format_    = sc.format;
    cfg_       = sc;

    auto dataCb = [this](float* data, uint32_t frames) -> uint32_t {
      return this->NativeDataCallback(data, frames);
    };
    auto errCb = [this](nwa::Status s, const std::string& msg) {
      this->NativeErrorCallback(s, msg);
    };

    nwa::Status s = engine_->Open(sc, dataCb, errCb);
    if (s != nwa::Status::Ok) {
      Napi::Error::New(env, std::string("Open failed: ") + nwa::StatusToString(s))
          .ThrowAsJavaScriptException();
      engine_.reset();
      return;
    }
    bufferFrames_ = sc.bufferFrames;
  }

  ~StreamWrap() {
    if (engine_) {
      engine_->Close();
      engine_.reset();
    }
    if (tsfn_) {
      tsfn_.Release();
      tsfn_ = nullptr;
    }
  }

 private:
  // Native audio callback - runs on the engine's worker thread.
  uint32_t NativeDataCallback(float* data, uint32_t frames) {
    if (direction_ == nwa::StreamDirection::Output) {
      // Pull from ring buffer.
      std::lock_guard<std::mutex> g(ringMutex_);
      size_t avail = (writePos_ - readPos_) & ringMask_;
      size_t toCopy = std::min<size_t>(avail, frames * channels_);
      if (toCopy < frames * channels_) {
        // Underrun: zero-fill the remainder.
        std::memset(data + toCopy, 0,
                    (frames * channels_ - toCopy) * sizeof(float));
        underruns_.fetch_add(1);
      }
      for (size_t i = 0; i < toCopy; ++i) {
        data[i] = ring_[readPos_];
        readPos_ = (readPos_ + 1) & ringMask_;
      }
      return frames;
    } else {
      // Input: push into ring buffer and signal JS.
      {
        std::lock_guard<std::mutex> g(ringMutex_);
        size_t freeSlots = ringMask_ - ((writePos_ - readPos_) & ringMask_);
        if (freeSlots < frames * channels_) {
          // Overrun: drop oldest samples.
          size_t drop = (frames * channels_) - freeSlots;
          readPos_ = (readPos_ + drop) & ringMask_;
          overruns_.fetch_add(1);
        }
        for (uint32_t i = 0; i < frames * channels_; ++i) {
          ring_[writePos_] = data[i];
          writePos_ = (writePos_ + 1) & ringMask_;
        }
      }
      // Non-blocking call into JS to deliver the chunk.
      if (tsfn_) {
        // Allocate a copy of the captured chunk and hand it to JS.
        auto* chunk = new std::vector<float>(data, data + frames * channels_);
        tsfn_.NonBlockingCall(chunk, [](Napi::Env env, Napi::Function cb,
                                        std::vector<float>* payload) {
          if (!payload) return;
          Napi::Float32Array arr = Napi::Float32Array::New(env, payload->size());
          std::memcpy(arr.Data(), payload->data(),
                      payload->size() * sizeof(float));
          cb.Call({ arr });
          delete payload;
        });
      }
      return frames;
    }
  }

  void NativeErrorCallback(nwa::Status s, const std::string& msg) {
    if (errTsfn_) {
      auto* payload = new std::pair<std::string, std::string>(
          nwa::StatusToString(s), msg);
      errTsfn_.NonBlockingCall(payload, [](Napi::Env env, Napi::Function cb,
                                            std::pair<std::string, std::string>* p) {
        cb.Call({ Napi::String::New(env, p->first),
                  Napi::String::New(env, p->second) });
        delete p;
      });
    }
  }

  // ----- JS methods --------------------------------------------------------
  Napi::Value Start(const Napi::CallbackInfo& info) {
    if (!engine_) return info.Env().Undefined();
    nwa::Status s = engine_->Start();
    if (s != nwa::Status::Ok) {
      Napi::Error::New(info.Env(), nwa::StatusToString(s))
          .ThrowAsJavaScriptException();
    }
    return info.Env().Undefined();
  }

  Napi::Value Stop(const Napi::CallbackInfo& info) {
    if (!engine_) return info.Env().Undefined();
    nwa::Status s = engine_->Stop();
    if (s != nwa::Status::Ok) {
      Napi::Error::New(info.Env(), nwa::StatusToString(s))
          .ThrowAsJavaScriptException();
    }
    return info.Env().Undefined();
  }

  Napi::Value Close(const Napi::CallbackInfo& info) {
    if (engine_) {
      engine_->Close();
      engine_.reset();
    }
    if (tsfn_) { tsfn_.Release(); tsfn_ = nullptr; }
    if (errTsfn_) { errTsfn_.Release(); errTsfn_ = nullptr; }
    return info.Env().Undefined();
  }

  Napi::Value Write(const Napi::CallbackInfo& info) {
    if (!engine_) return info.Env().Undefined();
    if (info.Length() < 1 || !info[0].IsTypedArray()) {
      Napi::TypeError::New(info.Env(), "Expected Float32Array")
          .ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }
    auto fa = info[0].As<Napi::Float32Array>();
    size_t n = fa.ElementLength();
    if (n == 0) return Napi::Number::New(info.Env(), 0);
    const float* p = fa.Data();
    std::lock_guard<std::mutex> g(ringMutex_);
    size_t freeSlots = ringMask_ - ((writePos_ - readPos_) & ringMask_);
    size_t toWrite = std::min(freeSlots, n);
    for (size_t i = 0; i < toWrite; ++i) {
      ring_[writePos_] = p[i];
      writePos_ = (writePos_ + 1) & ringMask_;
    }
    return Napi::Number::New(info.Env(), static_cast<double>(toWrite));
  }

  Napi::Value Read(const Napi::CallbackInfo& info) {
    if (!engine_) return info.Env().Null();
    std::lock_guard<std::mutex> g(ringMutex_);
    size_t avail = (writePos_ - readPos_) & ringMask_;
    if (avail == 0) return info.Env().Null();
    Napi::Float32Array out = Napi::Float32Array::New(info.Env(), avail);
    float* dst = out.Data();
    for (size_t i = 0; i < avail; ++i) {
      dst[i] = ring_[readPos_];
      readPos_ = (readPos_ + 1) & ringMask_;
    }
    return out;
  }

  Napi::Value Flush(const Napi::CallbackInfo& info) {
    std::lock_guard<std::mutex> g(ringMutex_);
    readPos_ = writePos_;
    return info.Env().Undefined();
  }

  Napi::Value Latency(const Napi::CallbackInfo& info) {
    if (!engine_) return Napi::Number::New(info.Env(), 0);
    uint32_t in = 0, out = 0;
    engine_->Latency(&in, &out);
    return Napi::Number::New(info.Env(), static_cast<double>(in + out));
  }

  Napi::Value Xruns(const Napi::CallbackInfo& info) {
    uint32_t total = underruns_.load() + overruns_.load() +
                     (engine_ ? engine_->XrunCount() : 0);
    return Napi::Number::New(info.Env(), static_cast<double>(total));
  }

  Napi::Value StreamTime(const Napi::CallbackInfo& info) {
    if (!engine_) return Napi::Number::New(info.Env(), 0);
    return Napi::Number::New(info.Env(), engine_->StreamTimeSeconds());
  }

  Napi::Value BufferedFrames(const Napi::CallbackInfo& info) {
    std::lock_guard<std::mutex> g(ringMutex_);
    size_t avail = (writePos_ - readPos_) & ringMask_;
    return Napi::Number::New(info.Env(),
                             static_cast<double>(avail) / channels_);
  }

  Napi::Value On(const Napi::CallbackInfo& info) {
    if (info.Length() < 2 || !info[0].IsString() || !info[1].IsFunction()) {
      Napi::TypeError::New(info.Env(), "Expected (eventName, callback)")
          .ThrowAsJavaScriptException();
      return info.Env().Undefined();
    }
    std::string ev = info[0].As<Napi::String>().Utf8Value();
    Napi::Function cb = info[1].As<Napi::Function>();
    if (ev == "data") {
      tsfn_ = Napi::ThreadSafeFunction::New(
          info.Env(), cb, "AudioDataTSFN", 0, 1);
    } else if (ev == "error") {
      errTsfn_ = Napi::ThreadSafeFunction::New(
          info.Env(), cb, "AudioErrorTSFN", 0, 1);
    } else {
      Napi::Error::New(info.Env(), "Unknown event: " + ev)
          .ThrowAsJavaScriptException();
    }
    return info.Env().Undefined();
  }

  // ----- Accessors ---------------------------------------------------------
  Napi::Value GetBackend(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), engine_ ? engine_->Name() : "");
  }
  Napi::Value GetDirection(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(),
        direction_ == nwa::StreamDirection::Input ? "input" : "output");
  }
  Napi::Value GetSampleRate(const Napi::CallbackInfo& info) {
    return Napi::Number::New(info.Env(), sampleRate_);
  }
  Napi::Value GetChannels(const Napi::CallbackInfo& info) {
    return Napi::Number::New(info.Env(), channels_);
  }
  Napi::Value GetFormat(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), FormatToString(format_));
  }
  Napi::Value GetBufferFrames(const Napi::CallbackInfo& info) {
    return Napi::Number::New(info.Env(), bufferFrames_);
  }

  // ----- State -------------------------------------------------------------
  std::unique_ptr<nwa::AudioEngine> engine_;
  nwa::Backend         backend_        = nwa::Backend::Wasapi;
  nwa::StreamDirection direction_      = nwa::StreamDirection::Output;
  nwa::SampleFormat    format_         = nwa::SampleFormat::F32;
  uint16_t             channels_       = 2;
  uint32_t             sampleRate_     = 48000;
  uint32_t             bufferFrames_   = 0;
  nwa::StreamConfig    cfg_;

  std::mutex          ringMutex_;
  std::vector<float>  ring_;
  size_t              readPos_  = 0;
  size_t              writePos_ = 0;
  size_t              ringMask_ = 0;

  std::atomic<uint32_t> underruns_{0};
  std::atomic<uint32_t> overruns_{0};

  Napi::ThreadSafeFunction tsfn_;
  Napi::ThreadSafeFunction errTsfn_;
};

// ---------------------------------------------------------------------------
// Module-level functions
// ---------------------------------------------------------------------------

Napi::Value ListDevices(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
    Napi::TypeError::New(env, "Expected (backend, direction)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  nwa::Backend b = ParseBackend(info[0].As<Napi::String>().Utf8Value());
  std::string dirStr = info[1].As<Napi::String>().Utf8Value();
  nwa::StreamDirection dir = (dirStr == "input")
      ? nwa::StreamDirection::Input : nwa::StreamDirection::Output;

  auto engine = CreateEngine(b);
  if (!engine) {
    Napi::Error::New(env, "Unsupported backend").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::vector<nwa::DeviceInfo> devices;
  nwa::Status s = engine->EnumerateDevices(dir, &devices);
  if (s != nwa::Status::Ok) {
    Napi::Error::New(env, std::string("EnumerateDevices failed: ") +
                          nwa::StatusToString(s)).ThrowAsJavaScriptException();
    return env.Undefined();
  }

  Napi::Array arr = Napi::Array::New(env, devices.size());
  for (size_t i = 0; i < devices.size(); ++i) {
    Napi::Object obj = Napi::Object::New(env);
    obj.Set("id",                Napi::String::New(env, devices[i].id));
    obj.Set("name",              Napi::String::New(env, devices[i].name));
    obj.Set("adapter",           Napi::String::New(env, devices[i].adapter));
    obj.Set("direction",         Napi::String::New(env,
                                dirStr == "input" ? "input" : "output"));
    obj.Set("maxInputChannels",  Napi::Number::New(env, devices[i].maxInputChannels));
    obj.Set("maxOutputChannels", Napi::Number::New(env, devices[i].maxOutputChannels));
    obj.Set("isDefaultInput",    Napi::Boolean::New(env, devices[i].isDefaultInput));
    obj.Set("isDefaultOutput",   Napi::Boolean::New(env, devices[i].isDefaultOutput));
    Napi::Array rates = Napi::Array::New(env, devices[i].supportedSampleRates.size());
    for (size_t j = 0; j < devices[i].supportedSampleRates.size(); ++j)
      rates[j] = Napi::Number::New(env, devices[i].supportedSampleRates[j]);
    obj.Set("supportedSampleRates", rates);
    Napi::Array fmts = Napi::Array::New(env, devices[i].supportedFormats.size());
    for (size_t j = 0; j < devices[i].supportedFormats.size(); ++j)
      fmts[j] = Napi::String::New(env, FormatToString(devices[i].supportedFormats[j]));
    obj.Set("supportedFormats", fmts);
    arr[i] = obj;
  }
  return arr;
}

Napi::Value IsFormatSupported(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  if (info.Length() < 1 || !info[0].IsObject()) {
    Napi::TypeError::New(env, "Expected config object")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }
  Napi::Object cfg = info[0].As<Napi::Object>();
  nwa::StreamConfig sc;
  auto engine = CreateEngine(ParseBackend(cfg.Has("backend")
      ? cfg.Get("backend").As<Napi::String>().Utf8Value() : "wasapi"));
  if (!engine) return Napi::Boolean::New(env, false);

  sc.direction = (cfg.Has("direction") &&
                  cfg.Get("direction").As<Napi::String>().Utf8Value() == "input")
                     ? nwa::StreamDirection::Input : nwa::StreamDirection::Output;
  if (cfg.Has("deviceId"))
    sc.deviceId = cfg.Get("deviceId").As<Napi::String>().Utf8Value();
  sc.sampleRate = cfg.Has("sampleRate")
      ? cfg.Get("sampleRate").As<Napi::Number>().Uint32Value() : 48000;
  sc.channels = cfg.Has("channels")
      ? static_cast<uint16_t>(cfg.Get("channels").As<Napi::Number>().Uint32Value()) : 2;
  sc.format = cfg.Has("format")
      ? ParseFormat(cfg.Get("format").As<Napi::String>().Utf8Value())
      : nwa::SampleFormat::F32;

  nwa::SampleFormatInfo nearest{};
  nwa::Status s = engine->IsFormatSupported(sc, &nearest);
  Napi::Object r = Napi::Object::New(env);
  r.Set("supported", Napi::Boolean::New(env, s == nwa::Status::Ok));
  Napi::Object nearObj = Napi::Object::New(env);
  nearObj.Set("format",     Napi::String::New(env, FormatToString(nearest.format)));
  nearObj.Set("sampleRate", Napi::Number::New(env, nearest.sampleRate));
  nearObj.Set("channels",   Napi::Number::New(env, nearest.channels));
  r.Set("nearest", nearObj);
  return r;
}

Napi::Value GetBackends(const Napi::CallbackInfo& info) {
  Napi::Env env = info.Env();
  Napi::Array arr = Napi::Array::New(env, 6);
  arr[uint32_t(0)] = Napi::String::New(env, "wasapi");
  arr[uint32_t(1)] = Napi::String::New(env, "wasapi-exclusive");
  arr[uint32_t(2)] = Napi::String::New(env, "asio");
  arr[uint32_t(3)] = Napi::String::New(env, "mme");
  arr[uint32_t(4)] = Napi::String::New(env, "wdm");
  arr[uint32_t(5)] = Napi::String::New(env, "audiograph");
  return arr;
}

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
  StreamWrap::Init(env, exports);
  exports.Set(Napi::String::New(env, "listDevices"),
              Napi::Function::New(env, ListDevices));
  exports.Set(Napi::String::New(env, "isFormatSupported"),
              Napi::Function::New(env, IsFormatSupported));
  exports.Set(Napi::String::New(env, "backends"),
              Napi::Function::New(env, GetBackends));
  return exports;
}

}  // namespace

NODE_API_MODULE(win_audio, InitAll)
