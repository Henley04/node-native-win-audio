'use strict';

/**
 * node-native-win-audio
 *
 * Native Node.js addon for low-latency Windows audio.  Provides multiple
 * backends: WASAPI (shared & exclusive), ASIO, MME, WDM-KS, and AudioGraph.
 *
 * The native module is built with node-gyp; on Windows the binary is loaded
 * from `build/Release/win_audio.node`.  On non-Windows platforms the require
 * will fail and the wrapper surfaces a friendly error.
 */

let native = null;
try {
  // node-gyp puts the binary here after `npm install`.
  native = require('../build/Release/win_audio.node');
} catch (e1) {
  try {
    native = require('../build/Debug/win_audio.node');
  } catch (e2) {
    native = null;
  }
}

/**
 * Backends supported by this addon.  Note: actual availability at runtime
 * depends on the host OS and installed drivers.
 *
 * @enum {string}
 */
const Backend = Object.freeze({
  WASAPI:           'wasapi',
  WASAPI_EXCLUSIVE: 'wasapi-exclusive',
  ASIO:             'asio',
  MME:              'mme',
  WDM:              'wdm',
  AUDIOGRAPH:       'audiograph',
});

/**
 * @enum {string}
 */
const Direction = Object.freeze({
  INPUT:  'input',
  OUTPUT: 'output',
});

/**
 * @enum {string}
 */
const Format = Object.freeze({
  U8:     'u8',
  S16:    's16',
  S24:    's24',
  S24_32: 's24-32',
  S32:    's32',
  F32:    'f32',
  F64:    'f64',
});

/**
 * Throws if the native addon is not loaded.  Used by every entry point.
 */
function ensureNative() {
  if (!native) {
    throw new Error(
      'node-native-win-audio native module not found. ' +
      'Run `npm install` (or `npm run build`) on a Windows machine with the ' +
      'Visual Studio C++ build tools and Windows SDK installed.'
    );
  }
}

/**
 * List audio devices available to a given backend.
 *
 * @param {string} backend   One of the Backend values.
 * @param {string} direction 'input' or 'output'.
 * @returns {Array<{id,name,adapter,direction,maxInputChannels,maxOutputChannels,isDefaultInput,isDefaultOutput,supportedSampleRates,supportedFormats}>}
 */
function listDevices(backend = Backend.WASAPI, direction = Direction.OUTPUT) {
  ensureNative();
  return native.listDevices(backend, direction);
}

/**
 * Probe whether a specific configuration is supported by the backend.
 *
 * @param {object} cfg Config with backend, direction, deviceId, sampleRate,
 *                     channels, format.
 * @returns {{supported: boolean, nearest: {format,sampleRate,channels}}}
 */
function isFormatSupported(cfg) {
  ensureNative();
  return native.isFormatSupported(cfg);
}

/**
 * List all backend identifiers known to this addon.  Availability at runtime
 * depends on the host.
 *
 * @returns {string[]}
 */
function backends() {
  ensureNative();
  return native.backends();
}

/**
 * Create an audio stream.
 *
 * @param {object} cfg
 * @param {string} [cfg.backend='wasapi']         Backend to use.
 * @param {string} [cfg.direction='output']       'input' or 'output'.
 * @param {string} [cfg.deviceId='']              Backend-specific device id
 *                                                (empty = default).
 * @param {number} [cfg.sampleRate=48000]         Sample rate in Hz.
 * @param {number} [cfg.channels=2]               Number of channels.
 * @param {string} [cfg.format='f32']             Sample format (the wrapper
 *                                                always converts to/from f32
 *                                                on the JS side).
 * @param {number} [cfg.bufferFrames=0]           0 = backend default.
 * @param {number} [cfg.numBuffers=0]             0 = backend default.
 * @param {boolean} [cfg.exclusive=false]         WASAPI exclusive mode flag.
 * @param {boolean} [cfg.eventDriven=true]        Use event-driven mode when
 *                                                backend supports it.
 * @returns {Stream}
 */
function createStream(cfg = {}) {
  ensureNative();
  const full = Object.assign({
    backend: Backend.WASAPI,
    direction: Direction.OUTPUT,
    deviceId: '',
    sampleRate: 48000,
    channels: 2,
    format: Format.F32,
    bufferFrames: 0,
    numBuffers: 0,
    exclusive: false,
    eventDriven: true,
  }, cfg);
  return new Stream(full);
}

/**
 * Stream wraps the native Stream object and exposes a friendlier API.  All
 * data movement is done with Float32Array (interleaved).
 *
 * - Output streams: call `write(Float32Array)` to push audio.  Use
 *   `bufferedFrames` to know how much is queued; top up before it drains.
 * - Input streams: register an `on('data', (float32Array) => {})` listener.
 *   The native side will invoke the callback (on a libuv thread) whenever a
 *   new chunk of audio is available.
 */
class Stream {
  constructor(cfg) {
    this._cfg = cfg;
    this._native = new native.Stream(cfg);
    this._closed = false;
    this._dataListeners = [];
    this._errorListeners = [];

    // Wire the native `on` API.  The native side delivers data via a
    // ThreadSafeFunction which runs on the JS main thread.
    this._native.on('data', (float32Array) => {
      for (const fn of this._dataListeners) {
        try { fn(float32Array); } catch (e) {
          process.nextTick(() => { throw e; });
        }
      }
    });
    this._native.on('error', (status, message) => {
      for (const fn of this._errorListeners) {
        try { fn(new Error(`[${status}] ${message}`)); } catch (e) {
          process.nextTick(() => { throw e; });
        }
      }
    });
  }

  /**
   * Register an event listener.  Supported events: 'data', 'error'.
   * @param {string} event
   * @param {Function} listener
   */
  on(event, listener) {
    if (typeof listener !== 'function') {
      throw new TypeError('listener must be a function');
    }
    if (event === 'data')  this._dataListeners.push(listener);
    else if (event === 'error') this._errorListeners.push(listener);
    else throw new Error(`Unknown event: ${event}`);
    return this;
  }

  /**
   * Remove a previously registered listener.
   */
  off(event, listener) {
    if (event === 'data') {
      this._dataListeners = this._dataListeners.filter(f => f !== listener);
    } else if (event === 'error') {
      this._errorListeners = this._errorListeners.filter(f => f !== listener);
    }
    return this;
  }

  /**
   * Begin streaming.
   */
  start() { this._native.start(); return this; }

  /**
   * Stop streaming (stream may be restarted with start()).
   */
  stop() { this._native.stop(); return this; }

  /**
   * Stop and release all native resources.
   */
  close() {
    if (this._closed) return;
    this._closed = true;
    this._dataListeners = [];
    this._errorListeners = [];
    this._native.close();
  }

  /**
   * Push interleaved float samples into the output ring buffer.
   * Returns the number of samples actually accepted (may be less than
   * requested if the ring is full).
   */
  write(float32Array) {
    return this._native.write(float32Array);
  }

  /**
   * Pull available input samples as a Float32Array (or null if empty).
   */
  read() {
    return this._native.read();
  }

  /**
   * Discard all queued samples.
   */
  flush() {
    this._native.flush();
    return this;
  }

  /** Total round-trip latency in frames. */
  latencyFrames() { return this._native.latency(); }

  /** Total round-trip latency in seconds. */
  latencySeconds() {
    return this._native.latency() / this._cfg.sampleRate;
  }

  /** Number of underruns + overruns since start(). */
  xruns() { return this._native.xruns(); }

  /** Stream clock time in seconds (since start). */
  streamTime() { return this._native.streamTime(); }

  /** Number of frames currently queued in the ring buffer. */
  bufferedFrames() { return this._native.bufferedFrames(); }

  /** Backend name as reported by the native layer. */
  get backend()     { return this._cfg.backend; }
  get direction()   { return this._cfg.direction; }
  get sampleRate()  { return this._cfg.sampleRate; }
  get channels()    { return this._cfg.channels; }
  get format()      { return this._cfg.format; }
  get bufferFrames(){ return this._cfg.bufferFrames; }
}

module.exports = {
  // High-level API
  listDevices,
  createStream,
  isFormatSupported,
  backends,
  // Enums
  Backend,
  Direction,
  Format,
  // Class (for instanceof checks)
  Stream,
};
