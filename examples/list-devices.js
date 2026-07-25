// Enumerate devices for every backend and report availability.
'use strict';

const audio = require('../lib');

console.log('Available backend identifiers:');
for (const b of audio.backends()) console.log('  -', b);

console.log('\nEnumerating devices per backend / direction:');
for (const backend of audio.backends()) {
  for (const dir of ['input', 'output']) {
    let devices = [];
    try {
      devices = audio.listDevices(backend, dir);
    } catch (e) {
      console.log(`\n[${backend} / ${dir}]  NOT AVAILABLE: ${e.message}`);
      continue;
    }
    if (devices.length === 0) {
      console.log(`\n[${backend} / ${dir}]  (no devices)`);
      continue;
    }
    console.log(`\n[${backend} / ${dir}]`);
    for (const d of devices) {
      const def = dir === 'input' ? d.isDefaultInput : d.isDefaultOutput;
      console.log(`  ${def ? '*' : ' '} ${d.name}`);
      console.log(`      id            = ${d.id}`);
      console.log(`      maxIn=${d.maxInputChannels} maxOut=${d.maxOutputChannels}`);
      console.log(`      rates=${d.supportedSampleRates.join(', ')}`);
      console.log(`      formats=${d.supportedFormats.join(', ')}`);
    }
  }
}
