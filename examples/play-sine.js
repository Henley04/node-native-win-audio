// Sine wave generator -> default WASAPI output device.
//
// Usage:
//   node examples/play-sine.js [freq] [durationSec]
'use strict';

const audio = require('../lib');

const freq = parseFloat(process.argv[2] || '440');
const durSec = parseFloat(process.argv[3] || '3');

const sr = 48000;
const stream = audio.createStream({
  backend: 'wasapi',
  direction: 'output',
  sampleRate: sr,
  channels: 2,
  format: 'f32',
});

const phase = [0, 0];
const CHUNK = 1024;

function fill(n) {
  const buf = new Float32Array(n * 2);
  for (let i = 0; i < n; ++i) {
    const s = Math.sin(phase[0]) * 0.2;
    phase[0] += (2 * Math.PI * freq) / sr;
    buf[i * 2]     = s;
    buf[i * 2 + 1] = s;
  }
  stream.write(buf);
}

// Prime the ring buffer with 200 ms.
fill(Math.floor(sr * 0.2 / CHUNK) * CHUNK + CHUNK);

stream.on('error', (e) => { console.error(e.message); process.exit(1); });
stream.start();

const startMs = Date.now();
const timer = setInterval(() => {
  if (Date.now() - startMs > durSec * 1000) {
    clearInterval(timer);
    stream.stop();
    stream.close();
    console.log(`Played ${freq} Hz for ${durSec} s.  xruns=${stream.xruns()}`);
    return;
  }
  const buffered = stream.bufferedFrames();
  if (buffered < sr / 10) fill(CHUNK);
}, 5);
