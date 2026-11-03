// Run after PicoSwitch/compress.js. Embed the same transport lines in the FAP.
import fs from 'node:fs';

const payload = fs.readFileSync(new URL('../PicoSwitch/payload.txt', import.meta.url), 'ascii');
const lines = payload.split(/\r?\n/).filter((line) => line.length > 0);
if (!lines.length) throw new Error('PicoSwitch/payload.txt is empty');
for (const [i, line] of lines.entries()) {
  if (!/^[\x20-\x7e]+$/.test(line)) throw new Error(`Unsupported character in payload line ${i + 1}`);
}
const source = [
  '// Generated from PicoSwitch/payload.txt by prepare_payload.mjs.',
  '#pragma once',
  `#define CIVSWITCH_PAYLOAD_LINE_COUNT ${lines.length}`,
  'static const char* const civswitch_payload[] = {',
  // Escape question marks with C's `\\?` escape to avoid trigraph parsing.
  ...lines.map((line) => `    ${JSON.stringify(line).replaceAll('?', '\\?')},`),
  '};',
  '',
].join('\n');
fs.writeFileSync(new URL('./payload.h', import.meta.url), source, 'ascii');
console.log(`Embedded ${lines.length} lines into FlipperApp/payload.h`);
