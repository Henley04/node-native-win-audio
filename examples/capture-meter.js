// Capture from default input device and print RMS meter.
//
// Usage:
//   node examples/capture-meter.js [durationSec]
'use strict';

const audio = require('../lib');

const durSec = parseFloat(process.argv[2] || '5');

const stream = audio.createStream({
  backend: 'wasapi',
  direction: 'input',
  sampleRate: 48000,
  channels: 1,
});

let count = 0;
stream.on('data', (data) => {
  let sum = 0;
  for (let i = 0; i < data.length; ++i) sum += data[i] * data[i];
  const rms = Math.sqrt(sum / data.length);
  const frames = data.length;
  count += frames;
  process.stdout.write(`\rrms=${rms.toFixed(4)}  frames=${frames}  total=${count}`);
});

stream.on('error', (e) => { console.error('\n' + e.message); process.exit(1); });

stream.start();

setTimeout(() => {
  stream.stop();
  stream.close();
  console.log(`\nCaptured ${count} frames.  xruns=${stream.xruns()}`);
}, durSec * 1000);
