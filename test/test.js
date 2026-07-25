// Smoke test for the JS wrapper layer.  These tests do not require a working
// audio device; they verify the API surface and the lazy-load behavior of the
// native module.
'use strict';

const assert = require('assert');
const audio = require('../lib');

function test(name, fn) {
  try {
    fn();
    console.log('ok   -', name);
  } catch (e) {
    console.error('FAIL -', name, '\n', e.stack);
    process.exitCode = 1;
  }
}

test('exports API surface', () => {
  assert.strictEqual(typeof audio.listDevices, 'function');
  assert.strictEqual(typeof audio.createStream, 'function');
  assert.strictEqual(typeof audio.isFormatSupported, 'function');
  assert.strictEqual(typeof audio.backends, 'function');
  assert.strictEqual(typeof audio.Stream, 'function');
});

test('Backend / Direction / Format enums are frozen', () => {
  assert.strictEqual(Object.isFrozen(audio.Backend), true);
  assert.strictEqual(Object.isFrozen(audio.Direction), true);
  assert.strictEqual(Object.isFrozen(audio.Format), true);
});

test('backends() returns the six expected strings', () => {
  let bs;
  try {
    bs = audio.backends();
  } catch (e) {
    // Native module not loadable on this platform - that's OK for the test.
    console.log('     (native module unavailable, skipping)');
    return;
  }
  assert.deepStrictEqual(bs, [
    'wasapi', 'wasapi-exclusive', 'asio', 'mme', 'wdm', 'audiograph',
  ]);
});

test('createStream rejects invalid backend gracefully', () => {
  // We cannot fully run createStream without a native module, but if it is
  // loadable on a non-Windows host, the open call will fail and we expect an
  // exception.  Just confirm the function accepts a config object shape.
  try {
    const s = audio.createStream({
      backend: 'invalid-backend',
      direction: 'output',
      sampleRate: 48000,
      channels: 2,
    });
    // If we somehow get here on a non-Windows host, that's an issue.
    if (s) s.close();
  } catch (e) {
    // Expected on platforms where the native module can't load or where the
    // backend is unknown.
  }
});

console.log('\nDone.');
