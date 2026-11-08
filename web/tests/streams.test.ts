// Streams (zgal.Stream) under Node: in memory, from a ReadableStream whose chunks the test gives one
// at a time (odd lengths, so reads span and split chunks), and from files through createNodeContext's
// loadAssetStream: ready, read, the end, a failing source and an asset that can't be opened.
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';

const assets = fileURLToPath(new URL('assets', import.meta.url));
let context: zgal.Context;
let scratch: string;

before(async () => {
  context = await createNodeContext([assets]);
  scratch = mkdtempSync(join(tmpdir(), 'zgal-streams-'));
});

after(() => {
  context.destroy();
  rmSync(scratch, { recursive: true });
});

// A stream whose source's chunks the test enqueues, ends or fails
function scripted(): { stream: zgal.Stream; source: ReadableStreamDefaultController<Uint8Array> } {
  let source!: ReadableStreamDefaultController<Uint8Array>;
  const readable = new ReadableStream<Uint8Array>({ start(controller) { source = controller; } });
  return { stream: zgal.readableStream(readable, 'scripted'), source };
}

// Bytes from start, counting up
function bytes(start: number, count: number): Uint8Array<ArrayBuffer> {
  return Uint8Array.from({ length: count }, (_, i) => (start + i) & 0xff);
}

// Whether a promise has settled, once pending callbacks have run
async function settled(promise: Promise<unknown>): Promise<boolean> {
  let done = false;
  promise.then(() => { done = true; }, () => { done = true; });
  await new Promise((resolve) => setTimeout(resolve, 10));
  return done;
}

// Everything a stream gives, read in pieces of size bytes as they arrive
async function readAll(stream: zgal.Stream, size: number): Promise<Uint8Array> {
  const parts: Uint8Array[] = [];
  for (;;) {
    await stream.ready(size);
    const part = stream.read(size);
    if (part === null) break;
    assert.ok(part.length > 0 && part.length <= size, `a read of ${size} gave ${part.length} bytes`);
    parts.push(part.slice());
  }
  return new Uint8Array(Buffer.concat(parts));
}

test('a stream gives what has arrived, across and within chunks, then ends', async () => {
  const { stream, source } = scripted();
  // Nothing yet: an empty read, and ready waits
  assert.deepEqual(stream.read(8), new Uint8Array(0));
  const ready = stream.ready(10);
  assert.equal(await settled(ready), false);
  source.enqueue(bytes(0, 7));
  assert.equal(await settled(ready), false);
  source.enqueue(bytes(7, 5));
  await ready;
  // Within the first chunk, then across both
  assert.deepEqual(stream.read(3), bytes(0, 3));
  assert.deepEqual(stream.read(0), new Uint8Array(0));
  assert.deepEqual(stream.read(6), bytes(3, 6));
  // At most what has arrived
  assert.deepEqual(stream.read(100), bytes(9, 3));
  assert.deepEqual(stream.read(4), new Uint8Array(0));
  // Empty chunks are skipped; ready(0) and a ready already met resolve at once
  source.enqueue(new Uint8Array(0));
  source.enqueue(bytes(12, 9));
  await stream.ready(9);
  await stream.ready(0);
  // Ending with fewer than asked resolves ready; the rest reads, then null, and stays null
  const end = stream.ready(20);
  source.close();
  await end;
  assert.deepEqual(stream.read(5), bytes(12, 5));
  assert.deepEqual(stream.read(5), bytes(17, 4));
  assert.equal(stream.read(5), null);
  await stream.ready(1);
  assert.equal(stream.read(1), null);
});

test('every reference to a stream shares one read position', async () => {
  const stream = zgal.readableStream(new Blob([bytes(0, 6)]).stream(), 'blob');
  const other = stream;
  await stream.ready(6);
  assert.deepEqual(stream.read(2), bytes(0, 2));
  assert.deepEqual(other.read(2), bytes(2, 2));
  assert.deepEqual(stream.read(2), bytes(4, 2));
});

test('a failing source rejects ready and fails reads, even with bytes waiting', async () => {
  const { stream, source } = scripted();
  source.enqueue(bytes(0, 5));
  await stream.ready(5);
  const waiting = stream.ready(10);
  source.error(new Error('the network went away'));
  await assert.rejects(waiting, /^Error: scripted: the network went away$/);
  await assert.rejects(stream.ready(1), /scripted: the network went away/);
  assert.throws(() => stream.read(5), /scripted: the network went away/);
  assert.throws(() => stream.read(5), /scripted: the network went away/);
});

test('a source giving anything but bytes fails', async () => {
  const readable = new ReadableStream<Uint8Array>({
    start(controller) { controller.enqueue('text' as unknown as Uint8Array); },
  });
  const stream = zgal.readableStream(readable, 'texts');
  await assert.rejects(stream.ready(1), /texts: gave a chunk that isn't a Uint8Array/);
});

test('sizes must be whole numbers', async () => {
  const stream = zgal.readableStream(new Blob([bytes(0, 4)]).stream(), 'blob');
  for (const size of [-1, 1.5, Number.NaN]) {
    assert.throws(() => stream.read(size), RangeError);
    await assert.rejects(stream.ready(size), RangeError);
  }
});

test('asset streams read files as loadAsset does, in pieces', async () => {
  // A file in the asset directory, by its relative path
  const texels = await context.loadAssetStream('texels.png');
  assert.deepEqual(await readAll(texels, 7), new Uint8Array(readFileSync(join(assets, 'texels.png'))));
  // An absolute path, larger than Node's 64 KiB reads, in odd pieces
  const large = join(scratch, 'large.bin');
  const expected = bytes(3, 200_001);
  writeFileSync(large, expected);
  assert.deepEqual(await readAll(await context.loadAssetStream(large), 4093), expected);
  // An empty file ends at once
  const empty = join(scratch, 'empty.bin');
  writeFileSync(empty, new Uint8Array(0));
  const none = await context.loadAssetStream(empty);
  await none.ready(4);
  assert.equal(none.read(4), null);
});

test('an asset stream that can\'t be opened fails when it\'s opened', async () => {
  await assert.rejects(context.loadAssetStream('missing.bin'), /^Error: can't open asset stream missing\.bin: not found in /);
  await assert.rejects(context.loadAssetStream(join(scratch, 'missing.bin')), /can't open asset stream .*missing\.bin: ENOENT/);
  const directory = join(scratch, 'directory');
  mkdirSync(directory);
  await assert.rejects(context.loadAssetStream(directory), /can't open asset stream .*directory: .* is a directory/);
});

test('a context given only whole assets streams none, and the other way round', async () => {
  const whole = await zgal.Context.create({ gpu: context.gpu, assets: async () => bytes(0, 4) });
  assert.deepEqual(await whole.loadAsset('a.bin'), bytes(0, 4));
  await assert.rejects(whole.loadAssetStream('a.bin'), /can't open asset stream a\.bin: this context streams no assets/);
  whole.destroy();
  const streamed = await zgal.Context.create({ gpu: context.gpu, assetStreams: async () => new Blob([bytes(0, 4)]).stream() });
  assert.deepEqual(await readAll(await streamed.loadAssetStream('a.bin'), 4), bytes(0, 4));
  await assert.rejects(streamed.loadAsset('a.bin'), /can't load asset a\.bin: this context reads no whole assets/);
  streamed.destroy();
});

test('cancelling a stream drops what waits, rejects waits, fails reads and cancels its source', async () => {
  let reason: unknown = 'not cancelled';
  let source!: ReadableStreamDefaultController<Uint8Array>;
  const readable = new ReadableStream<Uint8Array>({
    start(controller) { source = controller; },
    cancel(why) { reason = why; },
  });
  const stream = zgal.readableStream(readable, 'scripted');
  source.enqueue(bytes(0, 4));
  await stream.ready(4);
  const waiting = stream.ready(10);
  stream.cancel();
  await assert.rejects(waiting, /^Error: scripted: cancelled$/);
  assert.throws(() => stream.read(4), /scripted: cancelled/);
  await assert.rejects(stream.ready(1), /scripted: cancelled/);
  await new Promise((resolve) => setTimeout(resolve, 10));
  assert.equal(reason, undefined);
  // Again: nothing more happens
  stream.cancel();
  assert.throws(() => stream.read(4), /scripted: cancelled/);
});

test('peekStream reads a stream\'s start and gives it again with the rest', async () => {
  const { stream, source } = scripted();
  const peeking = zgal.peekStream(stream, 6);
  source.enqueue(bytes(0, 4));
  assert.equal(await settled(peeking), false);
  source.enqueue(bytes(4, 5));
  const { head, stream: replay } = await peeking;
  assert.deepEqual(head, bytes(0, 6));
  // Reads span the head and the rest; ready counts the head
  await replay.ready(9);
  assert.deepEqual(replay.read(4), bytes(0, 4));
  assert.deepEqual(replay.read(4), bytes(4, 4));
  const more = replay.ready(3);
  assert.equal(await settled(more), false);
  source.enqueue(bytes(9, 3));
  source.close();
  await more;
  assert.deepEqual(await readAll(replay, 5), bytes(8, 4));
  // A stream shorter than the peek: head holds what there was, then the stream ends
  const short = await zgal.peekStream(zgal.readableStream(new Blob([bytes(0, 3)]).stream(), 'blob'), 8);
  assert.deepEqual(short.head, bytes(0, 3));
  assert.deepEqual(await readAll(short.stream, 8), bytes(0, 3));
  // A failing stream rejects; cancelling the replay cancels the stream
  const failing = scripted();
  const failed = zgal.peekStream(failing.stream, 4);
  failing.source.error(new Error('gone'));
  await assert.rejects(failed, /scripted: gone/);
  const cancelled = scripted();
  cancelled.source.enqueue(bytes(0, 4));
  const peeked = await zgal.peekStream(cancelled.stream, 2);
  peeked.stream.cancel();
  assert.throws(() => peeked.stream.read(1), /scripted: cancelled/);
  assert.throws(() => cancelled.stream.read(1), /scripted: cancelled/);
});
