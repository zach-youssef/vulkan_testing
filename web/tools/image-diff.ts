// Compares two PNGs, as examples/image_filter/tests/image_diff.cpp does for the native programs, so
// the web's tests need no C++ build:
//     node web/tools/image-diff.ts <expected> <actual> <tolerance> [max differing fraction]
// Succeeds if they have the same size and no channel differs by more than tolerance (0-255), or,
// with a fraction, if at most that fraction of pixels do.
import { readFileSync } from 'node:fs';
import { decodePng } from '../zgal/png.ts';

const args = process.argv.slice(2);
if (args.length !== 3 && args.length !== 4) {
  console.error('usage: node web/tools/image-diff.ts <expected> <actual> <tolerance> [max differing fraction]');
  process.exit(2);
}
const [expected, actual] = args.slice(0, 2).map((path) => {
  try {
    return decodePng(readFileSync(path));
  } catch (error) {
    console.error(`image-diff: can't read '${path}': ${error instanceof Error ? error.message : String(error)}`);
    process.exit(1);
  }
});
const tolerance = Number(args[2]);
const maxFraction = args.length === 4 ? Number(args[3]) : 0;
if (expected.width !== actual.width || expected.height !== actual.height) {
  console.error(`image-diff: sizes differ: ${expected.width}x${expected.height} vs ${actual.width}x${actual.height}`);
  process.exit(1);
}
const pixels = expected.width * expected.height;
let differing = 0;
let largest = 0;
for (let i = 0; i < pixels; i++) {
  let worst = 0;
  for (let c = 0; c < 4; c++) worst = Math.max(worst, Math.abs(expected.rgba[i * 4 + c] - actual.rgba[i * 4 + c]));
  largest = Math.max(largest, worst);
  if (worst > tolerance) differing++;
}
const fraction = differing / pixels;
if (fraction > maxFraction) {
  console.error(`image-diff: ${differing} of ${pixels} pixels (${(fraction * 100).toFixed(3)}%) differ by more than ${tolerance} (largest ${largest})`);
  process.exit(1);
}
console.log(`image-diff: ${args[1]} matches ${args[0]} (largest difference ${largest})`);
