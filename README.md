# node-native-win-audio

A native Node.js addon for low-latency Windows audio I/O.  Provides multiple
backends with a single uniform API:

| Backend            | API                                       | Typical latency |
| ------------------ | ----------------------------------------- | --------------- |
| `wasapi`           | WASAPI (shared mode)                      | ~10 ms          |
| `wasapi-exclusive` | WASAPI (exclusive mode)                   | ~3-5 ms         |
| `asio`             | ASIO (Steinberg, dynamically loaded)      | ~1-3 ms         |
| `mme`              | MME / `winmm` (`waveIn`/`waveOut`)        | ~30-100 ms      |
| `wdm`              | WDM Kernel Streaming (`DirectKS`)         | ~5-10 ms        |
| `audiograph`       | WinRT `Windows.Media.Audio.AudioGraph`    | ~10 ms          |

The addon is built with [node-addon-api](https://github.com/nodejs/node-addon-api)
(N-API v8) and the Windows 10 SDK.

## Requirements

* **Windows 10 1809+** (Windows 11 also fine)
* **Node.js 14+** (N-API v8)
* **Visual Studio 2019 / 2022** with the "Desktop development with C++"
  workload, including the **Windows 10/11 SDK**
* For ASIO: a vendor-supplied ASIO driver installed (e.g. from Focusrite,
  RME, Steinberg, etc.).  The addon enumerates drivers registered under
  `HKLM\SOFTWARE\ASIO`.

## Installation

```bash
npm install node-native-win-audio
```

The package's `install` script runs `node-gyp rebuild` automatically.

## Quick start

### Render a sine wave to the default WASAPI device

```js
const audio = require('node-native-win-audio');

const sr = 48000;
const stream = audio.createStream({
  backend: 'wasapi',
  direction: 'output',
  sampleRate: sr,
  channels: 2,
  format: 'f32',
});

stream.start();

// Pre-fill the ring buffer with ~200 ms of audio.
const phase = new Float32Array(2);
const fill = (n) => {
  const buf = new Float32Array(n * 2);
  for (let i = 0; i < n; ++i) {
    const s = Math.sin(phase[0]) * 0.2;
    phase[0] += (2 * Math.PI * 440) / sr;
    buf[i * 2]     = s;
    buf[i * 2 + 1] = s;
  }
  stream.write(buf);
};

fill(sr / 5);  // prime

setInterval(() => {
  const buffered = stream.bufferedFrames();
  if (buffered < sr / 10) fill(sr / 20);   // top up
}, 10);

process.on('SIGINT', () => { stream.stop(); stream.close(); process.exit(0); });
```

### Capture from default input device

```js
const audio = require('node-native-win-audio');

const stream = audio.createStream({
  backend: 'wasapi',
  direction: 'input',
  sampleRate: 48000,
  channels: 1,
});

stream.on('data', (float32Array) => {
  // RMS for metering
  let sum = 0;
  for (let i = 0; i < float32Array.length; ++i) sum += float32Array[i] ** 2;
  const rms = Math.sqrt(sum / float32Array.length);
  console.log('rms:', rms.toFixed(4), 'frames:', float32Array.length / 1);
});

stream.start();
```

### Enumerate devices

```js
const audio = require('node-native-win-audio');

console.log('Backends:', audio.backends());

for (const backend of audio.backends()) {
  for (const dir of ['input', 'output']) {
    try {
      const devices = audio.listDevices(backend, dir);
      if (devices.length) {
        console.log(`\n[${backend} / ${dir}]`);
        for (const d of devices) {
          console.log(`  - ${d.name}  (default=${dir === 'input' ? d.isDefaultInput : d.isDefaultOutput})`);
        }
      }
    } catch (e) {
      console.log(`  ${backend}/${dir} not available: ${e.message}`);
    }
  }
}
```

## API reference

### `audio.listDevices(backend, direction)` → `DeviceInfo[]`

Enumerate the devices known to a particular backend.  `backend` is one of the
strings returned by `audio.backends()`, `direction` is `'input'` or `'output'`.

### `audio.isFormatSupported(config)` → `{ supported: boolean, nearest: {...} }`

Probe a configuration without opening the device.

### `audio.createStream(config)` → `Stream`

Open a new stream.  Config fields:

| Field          | Default   | Notes                                                  |
| -------------- | --------- | ------------------------------------------------------ |
| `backend`      | `'wasapi'`| One of the backend strings.                            |
| `direction`    | `'output'`| `'input'` or `'output'`.                               |
| `deviceId`     | `''`      | Backend-specific device id; empty = default.           |
| `sampleRate`   | `48000`   | Hz.                                                    |
| `channels`     | `2`       | Number of interleaved channels.                        |
| `format`       | `'f32'`   | Sample format used internally; JS always sees float.   |
| `bufferFrames` | `0`       | 0 = backend default.                                   |
| `numBuffers`   | `0`       | 0 = backend default.                                   |
| `exclusive`    | `false`   | WASAPI exclusive mode (use `wasapi-exclusive` instead).|
| `eventDriven`  | `true`    | Use event-driven mode when supported.                  |

### `Stream` methods

* `start()` / `stop()` / `close()` — control streaming.
* `write(Float32Array)` — push interleaved float samples (output streams).
* `read()` → `Float32Array | null` — pull captured samples (input streams).
* `flush()` — discard queued samples.
* `on('data', fn)` / `on('error', fn)` — register listeners (input streams
  deliver chunks via `'data'`).
* `latencyFrames()` / `latencySeconds()` — current round-trip latency.
* `xruns()` — total underruns + overruns since start.
* `streamTime()` — stream clock seconds since start.
* `bufferedFrames()` — frames currently queued in the ring buffer.

## Architecture

```
┌─────────────────────────── JS (Node main thread) ──────────────────┐
│                                                                    │
│   lib/index.js  ── wraps ──>  native.Stream  (Napi::ObjectWrap)     │
│                                                                    │
│   - stream.write(Float32Array)  ──>  ring buffer (JS → native)      │
│   - stream.on('data', cb)       <──  ThreadSafeFunction (input)     │
│                                                                    │
└────────────────────────────────┬───────────────────────────────────┘
                                 │
                                 │  N-API boundary
                                 │
┌────────────────────────────────▼───────────────────────────────────┐
│                          C++ native addon                          │
│                                                                    │
│   AudioEngine (abstract) ── implemented by ──┐                     │
│        │                                      │                     │
│        ├── WasapiEngine (shared + exclusive)  │                     │
│        ├── MmeEngine                          │                     │
│        ├── WdmEngine  (Kernel Streaming)      │                     │
│        ├── AsioEngine (dynamic COM)           │                     │
│        └── AudioGraphEngine (WinRT)           │                     │
│                                                                    │
│   Each engine owns its own worker thread, boosted to "Pro Audio"   │
│   MMCSS priority.  Data path is always interleaved float32.        │
│                                                                    │
└────────────────────────────────────────────────────────────────────┘
```

### Latency and the JS ring buffer

Real-time audio callbacks cannot be served directly from the JS main thread
because V8 garbage collection pauses would cause dropouts.  This addon uses a
ring buffer between the native audio thread and JS:

* **Output**: JS pushes interleaved float samples via `write()`.  The native
  audio thread drains the ring; on underrun it emits silence and bumps the
  xrun counter.  JS should keep the buffer ~50–200 ms full to absorb GC
  pauses.
* **Input**: The native audio thread accumulates captured samples and emits
  them as `Float32Array` chunks to the `'data'` listener via a
  `Napi::ThreadSafeFunction`.  JS receives the chunks on the libuv thread
  pool.

This means **practical end-to-end latency is bounded by the ring buffer
depth**, not the audio backend's own latency.  For absolute minimum latency
use a small ring buffer and ensure the JS producer is faster than real-time
(pre-rendering audio is the safest option).

### Backend notes

* **WASAPI shared** — works on every Windows 10+ machine.  Latency is
  typically 10 ms but can be lowered via the audio engine's "minimum period"
  if the user opens the Sound control panel.

* **WASAPI exclusive** — opens the endpoint in exclusive mode.  Requires the
  device to be free (no other app is using it).  The buffer size is chosen to
  match the device's minimum period for minimum latency.

* **ASIO** — dynamically loads the ASIO driver registered in the registry.
  Only one ASIO stream may be active per process (ASIO driver limitation).
  Sample format conversion covers all common ASIO sample types
  (Int16/24/32 LSB, Int32LSB16-24, Float32/64 LSB).

* **MME** — the legacy `waveOut`/`waveIn` API.  Reliable but high latency
  (~30-100 ms).  Useful as a fallback.

* **WDM-KS** — talks directly to WDM audio filter drivers via KS property
  requests.  Same latency tier as WASAPI exclusive on most hardware.  The
  implementation tries pin factories 0..15 and accepts the first one that
  supports the requested data format.

* **AudioGraph** — wraps `Windows.Media.Audio.AudioGraph`.  Always uses
  32-bit float internally.  Requires `QuantumSizeSelectionMode::LowestLatency`
  to be honored by the system.

## License

MIT.  See `LICENSE`.

## Acknowledgements

The ASIO host implementation here is written from scratch against the public
ASIO interface contract; it does not include or redistribute the Steinberg
ASIO SDK.
