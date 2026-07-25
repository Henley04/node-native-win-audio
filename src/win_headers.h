// win_headers.h - Single source of truth for Windows SDK header inclusion.
//
// The Windows audio SDK headers (mmsystem.h, ks.h, ksmedia.h, audioclient.h)
// have notoriously finicky include-order requirements:
//
//   * ks.h must precede ksmedia.h (#error otherwise).
//   * mmreg.h must precede ks.h so WAVEFORMATEXTENSIBLE is visible to
//     ksmedia.h's KSDATAFORMAT_SUBTYPE_* definitions.
//   * windows.h must precede mmsystem.h: mmsyscom.h references MMVERSION /
//     HDRVR / FAR / DRVCALLBACK types that windows.h is expected to define.
//   * objbase.h must be visible before audioclient.h / mmdeviceapi.h so the
//     COM GUID machinery (__uuidof, DEFINE_GUID) is in scope.
//   * WIN32_LEAN_AND_MEAN must NOT be defined: it trims out the mmsystem /
//     ksmedia type definitions and triggers C4430/C3646 errors from inside
//     the SDK headers themselves.
//
// On top of that, the Windows 10 SDK (10.0.26100) skips several composite
// SPEAKER_* channel-mask macros under certain configurations, so we provide
// per-macro #ifndef fallbacks (a single outer #ifndef doesn't work because
// the individual SPEAKER_FRONT_* macros are already defined, which would
// hide the missing composites).
//
// Every backend translation unit includes this header FIRST, before any
// project headers, to guarantee a consistent prelude.
#ifndef NODE_WIN_AUDIO_WIN_HEADERS_H_
#define NODE_WIN_AUDIO_WIN_HEADERS_H_

#ifndef NOMINMAX
#define NOMINMAX
#endif
// WIN32_LEAN_AND_MEAN is intentionally NOT defined - see file comment.

#include <windows.h>
#include <objbase.h>        // COM GUID machinery (__uuidof, DEFINE_GUID)

// mmreg.h carries WAVEFORMATEXTENSIBLE / WAVE_FORMAT_EXTENSIBLE and must be
// seen by ksmedia.h before it emits KSDATAFORMAT_SUBTYPE_*.
#include <mmreg.h>

// ks.h must precede ksmedia.h (#error KSMEDIA.H requires KS.H otherwise).
#include <ks.h>
#include <ksmedia.h>

// mmsystem.h (MME / winmm) pulls in mmsyscom.h which needs windows.h first.
#include <mmsystem.h>

// WASAPI headers - included here so backend .cc files don't need to repeat
// the ordering dance.  Harmless on backends that don't use WASAPI.
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>  // PKEY_Device_FriendlyName
#include <avrt.h>                            // AvSetMmThreadCharacteristics

// ---------------------------------------------------------------------------
// Defensive fallbacks for composite SPEAKER_* channel-mask macros.
//
// On the Windows 10 SDK (10.0.26100) ksmedia.h emits the individual
// SPEAKER_FRONT_LEFT / SPEAKER_FRONT_RIGHT / ... position macros but skips
// the composite SPEAKER_MONO / SPEAKER_STEREO / SPEAKER_QUAD / SPEAKER_5POINT1
// / SPEAKER_7POINT1 macros that BuildWaveFormat() needs.  A single outer
// #ifndef guard does NOT work because SPEAKER_FRONT_LEFT is already defined,
// which would hide the missing composites - so every macro gets its own
// #ifndef.
// ---------------------------------------------------------------------------
#ifndef SPEAKER_FRONT_LEFT
#define SPEAKER_FRONT_LEFT             0x1
#endif
#ifndef SPEAKER_FRONT_RIGHT
#define SPEAKER_FRONT_RIGHT            0x2
#endif
#ifndef SPEAKER_FRONT_CENTER
#define SPEAKER_FRONT_CENTER           0x4
#endif
#ifndef SPEAKER_LOW_FREQUENCY
#define SPEAKER_LOW_FREQUENCY          0x8
#endif
#ifndef SPEAKER_BACK_LEFT
#define SPEAKER_BACK_LEFT              0x10
#endif
#ifndef SPEAKER_BACK_RIGHT
#define SPEAKER_BACK_RIGHT             0x20
#endif
#ifndef SPEAKER_FRONT_LEFT_OF_CENTER
#define SPEAKER_FRONT_LEFT_OF_CENTER   0x40
#endif
#ifndef SPEAKER_FRONT_RIGHT_OF_CENTER
#define SPEAKER_FRONT_RIGHT_OF_CENTER  0x80
#endif
#ifndef SPEAKER_BACK_CENTER
#define SPEAKER_BACK_CENTER            0x100
#endif
#ifndef SPEAKER_SIDE_LEFT
#define SPEAKER_SIDE_LEFT              0x200
#endif
#ifndef SPEAKER_SIDE_RIGHT
#define SPEAKER_SIDE_RIGHT             0x400
#endif
#ifndef SPEAKER_TOP_CENTER
#define SPEAKER_TOP_CENTER             0x800
#endif
#ifndef SPEAKER_TOP_FRONT_LEFT
#define SPEAKER_TOP_FRONT_LEFT         0x1000
#endif
#ifndef SPEAKER_TOP_FRONT_CENTER
#define SPEAKER_TOP_FRONT_CENTER       0x2000
#endif
#ifndef SPEAKER_TOP_FRONT_RIGHT
#define SPEAKER_TOP_FRONT_RIGHT        0x4000
#endif
#ifndef SPEAKER_TOP_BACK_LEFT
#define SPEAKER_TOP_BACK_LEFT          0x8000
#endif
#ifndef SPEAKER_TOP_BACK_CENTER
#define SPEAKER_TOP_BACK_CENTER        0x10000
#endif
#ifndef SPEAKER_TOP_BACK_RIGHT
#define SPEAKER_TOP_BACK_RIGHT         0x20000
#endif

#ifndef SPEAKER_MONO
#define SPEAKER_MONO                   SPEAKER_FRONT_CENTER
#endif
#ifndef SPEAKER_STEREO
#define SPEAKER_STEREO                 (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
#endif
#ifndef SPEAKER_2POINT1
#define SPEAKER_2POINT1                (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_LOW_FREQUENCY)
#endif
#ifndef SPEAKER_3POINT1
#define SPEAKER_3POINT1                (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY)
#endif
#ifndef SPEAKER_QUAD
#define SPEAKER_QUAD                   (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#endif
#ifndef SPEAKER_SURROUND
#define SPEAKER_SURROUND               (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_BACK_CENTER)
#endif
#ifndef SPEAKER_5POINT1
#define SPEAKER_5POINT1                (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
#endif
#ifndef SPEAKER_5POINT1_SURROUND
#define SPEAKER_5POINT1_SURROUND       (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
#endif
#ifndef SPEAKER_7POINT1
#define SPEAKER_7POINT1                (SPEAKER_5POINT1 | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT)
#endif
#ifndef SPEAKER_7POINT1_SURROUND
#define SPEAKER_7POINT1_SURROUND       (SPEAKER_5POINT1 | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT | SPEAKER_FRONT_LEFT_OF_CENTER | SPEAKER_FRONT_RIGHT_OF_CENTER)
#endif
#ifndef SPEAKER_ALL
#define SPEAKER_ALL                    0xFFFFFFFF
#endif

#endif  // NODE_WIN_AUDIO_WIN_HEADERS_H_
