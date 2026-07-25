// util.h - Common helpers used across all backends.
#ifndef NODE_WIN_AUDIO_UTIL_H_
#define NODE_WIN_AUDIO_UTIL_H_

// Prevent windows.h from polluting the global namespace with `min`/`max`
// macros.  We rely on std::min/std::max throughout the codebase.
#ifndef NOMINMAX
#define NOMINMAX
#endif
// NOTE: WIN32_LEAN_AND_MEAN is intentionally NOT defined here.  Several
// backends need mmsystem.h / mmsyscom.h / ksmedia.h, whose type definitions
// (MMVERSION, HDRVR, DRVCALLBACK, KSDATAFORMAT_*) get gated out by
// WIN32_LEAN_AND_MEAN, producing C4430 / C3646 / C2065 errors from inside
// the SDK headers themselves.  Trimming windows.h saves ~10% build time but
// isn't worth the fragility.

#include <windows.h>

#include <string>
#include <vector>

#include "audio_types.h"

namespace nwa {

// Convert a Win32 HRESULT into our Status enum.  Inspects common audio-related
// error codes (AUDCLNT_*, ASIO-style, etc.).
Status HresultToStatus(HRESULT hr);

// Convert an HRESULT to a readable string.  Tries FormatMessageW first, then
// falls back to a hex representation if no message is available.
std::string HresultToString(HRESULT hr);

// Convert a wide string to UTF-8.
std::string WideToUtf8(const wchar_t* w, int len = -1);

// Convert a UTF-8 string to a wide string.
std::wstring Utf8ToWide(const std::string& s);

// Format a 32-bit WAVEFORMAT tag from a SampleFormat.
uint16_t SampleFormatToWaveFormatTag(SampleFormat f);

// Number of bytes per frame for the given config.
inline size_t BytesPerFrame(SampleFormat f, uint16_t channels) {
  return BytesPerSample(f) * channels;
}

// Convert any interleaved PCM sample format into float32 (output buffer must
// have at least frames*channels floats).  No-op when src is already F32.
void ConvertToFloat(const void* src, uint32_t frames, uint16_t channels,
                    SampleFormat srcFmt, float* dst);

// Convert float32 back into any interleaved PCM sample format.
void ConvertFromFloat(const float* src, uint32_t frames, uint16_t channels,
                      SampleFormat dstFmt, void* dst);

// Raises thread priority to "Pro Audio" (MMCSS).  Returns the task handle on
// success (to be passed to EndMmcssThread later), or nullptr on failure.
HANDLE BeginMmcssThread(const wchar_t* taskName = L"Pro Audio");

// Reverts MMCSS priority boost.  Safe to call with nullptr.
void EndMmcssThread(HANDLE task);

// Spin-wait helper used by event-driven loops when high precision is needed.
void YieldSpin();

}  // namespace nwa

#endif  // NODE_WIN_AUDIO_UTIL_H_
