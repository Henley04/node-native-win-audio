// asio_types.h - Minimal ASIO type declarations.
//
// These types mirror the public ASIO interface contract described in the
// Steinberg ASIO SDK documentation.  They are written from scratch (not copied
// from the SDK) to avoid redistributing the licensed SDK headers.  The driver
// DLLs from audio interface vendors use the same binary layout, so loading
// them dynamically at runtime works regardless of SDK availability.

#ifndef NODE_WIN_AUDIO_ASIO_TYPES_H_
#define NODE_WIN_AUDIO_ASIO_TYPES_H_

#include <cstdint>

namespace nwa {

// ASIO uses C-style integers / typedefs.
typedef int32_t          ASIOBool;
typedef double           ASIOSampleRate;
typedef int64_t          ASIOSamplePosition;
typedef int32_t          ASIOError;
typedef int32_t          ASIOSampleType;
typedef int32_t          ASIOMessageId;

// Standard ASIO error codes.
constexpr ASIOError ASE_OK               = 0;
constexpr ASIOError ASE_SUCCESS          = 0x3F;
constexpr ASIOError ASE_NotPresent       = -1000;
constexpr ASIOError ASE_HWMalfunction    = -999;
constexpr ASIOError ASE_InvalidParameter = -998;
constexpr ASIOError ASE_InvalidMode      = -997;
constexpr ASIOError ASE_SPNotAdvancing   = -996;
constexpr ASIOError ASE_NoClock          = -995;
constexpr ASIOError ASE_NoMemory         = -994;

// ASIO sample types (subset).
constexpr ASIOSampleType ASIOST_Int16MSB   = 0;
constexpr ASIOSampleType ASIOST_Int24MSB   = 1;
constexpr ASIOSampleType ASIOST_Int32MSB   = 2;
constexpr ASIOSampleType ASIOST_Float32MSB = 4;
constexpr ASIOSampleType ASIOST_Float64MSB = 5;
constexpr ASIOSampleType ASIOST_Int32MSB16 = 6;
constexpr ASIOSampleType ASIOST_Int32MSB18 = 7;
constexpr ASIOSampleType ASIOST_Int32MSB20 = 8;
constexpr ASIOSampleType ASIOST_Int32MSB24 = 9;
constexpr ASIOSampleType ASIOST_Int16LSB   = 16;
constexpr ASIOSampleType ASIOST_Int24LSB   = 17;
constexpr ASIOSampleType ASIOST_Int32LSB   = 18;
constexpr ASIOSampleType ASIOST_Float32LSB = 19;
constexpr ASIOSampleType ASIOST_Float64LSB = 20;
constexpr ASIOSampleType ASIOST_Int32LSB16 = 21;
constexpr ASIOSampleType ASIOST_Int32LSB18 = 22;
constexpr ASIOSampleType ASIOST_Int32LSB20 = 23;
constexpr ASIOSampleType ASIOST_Int32LSB24 = 24;
constexpr ASIOSampleType ASIOST_LastEntry  = 25;

#pragma pack(push, 4)
struct ASIOTimeStamp {
  uint32_t lo;
  uint32_t hi;
};
#pragma pack(pop)

struct AsioTimeInfo {
  double          speed;
  ASIOTimeStamp   systemTime;
  ASIOSamplePosition samplePosition;
  ASIOSampleRate  sampleRate;
  ASIOBool        flags;
  char            reserved[12];
};

struct ASIOTime {
  AsioTimeInfo  timeInfo;
  ASIOBool      timeCode;
  char          reserved[128];
};

struct ASIOBufferInfo {
  ASIOBool  isInput;     // input  = 1, output = 0
  int32_t   channelNum;
  void*     buffers[2];  // double-buffered
};

struct ASIOChannelInfo {
  int32_t       channel;
  ASIOBool      isInput;
  ASIOBool      isActive;
  int32_t       channelGroup;
  ASIOSampleType type;
  char          name[32];
};

struct ASIOCallbacks {
  void (*bufferSwitch)(long doubleBufferIndex, ASIOBool directProcess);
  void (*sampleRateDidChange)(ASIOSampleRate sRate);
  long (*asioMessage)(long selector, long value, void* message, double* opt);
  ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* time, long doubleBufferIndex,
                                    ASIOBool directProcess);
};

// ASIO message selectors.
constexpr ASIOMessageId kAsioSelectorSupported    = 1;
constexpr ASIOMessageId kAsioEngineVersion        = 2;
constexpr ASIOMessageId kAsioResetRequest         = 3;
constexpr ASIOMessageId kAsioBufferSizeChange     = 4;
constexpr ASIOMessageId kAsioResyncRequest        = 5;
constexpr ASIOMessageId kAsioLatenciesChanged     = 6;
constexpr ASIOMessageId kAsioSupportsTimeInfo     = 7;
constexpr ASIOMessageId kAsioSupportsTimeCode     = 8;
constexpr ASIOMessageId kAsioMMCCommand           = 9;
constexpr ASIOMessageId kAsioSupportsInputMonitor = 10;
constexpr ASIOMessageId kAsioSupportsInputGain    = 11;
constexpr ASIOMessageId kAsioSupportsInputMeter   = 12;
constexpr ASIOMessageId kAsioSupportsOutputGain   = 13;
constexpr ASIOMessageId kAsioSupportsOutputMeter  = 14;
constexpr ASIOMessageId kAsioOverload             = 15;

// The IASIO interface - a C++ abstract class with virtual functions.  The vtable
// layout is fixed by the ASIO ABI and matches every shipping driver.
class IASIO {
 public:
  virtual ASIOBool   Init(void* sysRef) = 0;
  virtual void       GetDriverName(char* name) = 0;
  virtual long       GetDriverVersion() = 0;
  virtual void       GetErrorMessage(char* str) = 0;
  virtual ASIOError  Start() = 0;
  virtual ASIOError  Stop() = 0;
  virtual ASIOError  GetChannels(long* numInput, long* numOutput) = 0;
  virtual ASIOError  GetLatencies(long* inputLatency, long* outputLatency) = 0;
  virtual ASIOError  GetBufferSize(long* minSize, long* maxSize,
                                   long* preferredSize, long* granularity) = 0;
  virtual ASIOError  CanSampleRate(ASIOSampleRate sampleRate) = 0;
  virtual ASIOError  GetSampleRate(ASIOSampleRate* currentRate) = 0;
  virtual ASIOError  SetSampleRate(ASIOSampleRate sampleRate) = 0;
  virtual ASIOError  GetClockSources(void* clocks, long* numSources) = 0;
  virtual ASIOError  SetClockSource(long index) = 0;
  virtual ASIOError  GetSamplePosition(ASIOSamplePosition* pos,
                                       ASIOSamplePosition* timestamp) = 0;
  virtual ASIOError  GetChannelInfo(ASIOChannelInfo* info) = 0;
  virtual ASIOError  CreateBuffers(ASIOBufferInfo* channelInfos,
                                   long numChannels, long bufferSize,
                                   ASIOCallbacks* callbacks) = 0;
  virtual ASIOError  DisposeBuffers() = 0;
  virtual ASIOError  ControlPanel() = 0;
  virtual ASIOError  Future(long selector, void* opt) = 0;
  virtual ASIOError  OutputMessage() = 0;
};

}  // namespace nwa

#endif  // NODE_WIN_AUDIO_ASIO_TYPES_H_
