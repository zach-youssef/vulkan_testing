// What running programs headless in a page shares with Node, checked without a GPU: the page's PNG
// encoder (CompressionStream) writes the same pixels as Node's (zlib), and the command line runs
// a program without a window headless in a page (runHeadless) but not in a canvas (mount).
// web/tests/headless.html runs image_filter in a browser.
import assert from 'node:assert/strict';
import { test } from 'node:test';
import * as zgal from '../zgal/index.ts';
import { decodePng, encodePng } from '../zgal/png.ts';
import { encodePngStream } from '../zgal/png-write.ts';
import { program } from '../image_filter.zgal.main.ts';

test('the page\'s PNG encoder writes what Node\'s does', async () => {
  const [width, height] = [5, 3];
  const rgba = new Uint8Array(width * height * 4).map((_, i) => (i * 37) % 256);
  const page = decodePng(await encodePngStream(width, height, rgba));
  const node = decodePng(encodePng(width, height, rgba));
  assert.deepEqual([page.width, page.height], [width, height]);
  assert.deepEqual(page.rgba, rgba);
  assert.deepEqual(node.rgba, rgba);
  await assert.rejects(encodePngStream(width, height, rgba.subarray(4)), /5x3/);
});

test('a program without a window runs headless in a page, not in a canvas', () => {
  const args = ['photo.png', 'out.png', '--blur', '2'];
  const launch = zgal.parseCommandLine(program, [...args], program.defaults(), 'browser');
  assert.ok(launch instanceof zgal.Launch);
  assert.equal(launch.frames, 1);
  assert.equal(launch.capture, zgal.commandLinePath('out.png')); // as the program's path parameters are
  assert.equal(launch.window, false);
  const canvas = zgal.parseCommandLine(program, [...args], program.defaults(), 'canvas');
  assert.ok(!(canvas instanceof zgal.Launch));
  assert.equal(canvas.status, 2);
  assert.match(canvas.text, /runs headless only/);
});

test('a program with a window runs headless in a page given --frames', () => {
  const windowed = { ...program, window: { title: 'x', width: 8, height: 4 }, captureTo: null, captureOption: true };
  const inputs = () => windowed.defaults();
  const args = ['photo.png', 'out.png'];
  const missing = zgal.parseCommandLine(windowed, [...args], inputs(), 'browser');
  assert.ok(!(missing instanceof zgal.Launch));
  assert.match(missing.text, /give --frames N/);
  const launch = zgal.parseCommandLine(windowed, [...args, '--frames', '3', '--capture', 'shot.png'], inputs(), 'browser');
  assert.ok(launch instanceof zgal.Launch);
  assert.deepEqual([launch.frames, launch.capture, launch.size], [3, 'shot.png', [8, 4]]);
  const canvas = zgal.parseCommandLine(windowed, [...args, '--capture', 'shot.png'], inputs(), 'canvas');
  assert.ok(!(canvas instanceof zgal.Launch));
  assert.match(canvas.text, /--capture runs headless/);
});
