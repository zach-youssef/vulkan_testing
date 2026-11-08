// Stream buffers (RenderGraph.createStreamBuffer): frames write whole words of what has arrived, at
// most bytesPerFrame each, from chunks of uneven lengths; a stream that fails, gives too much or ends
// early fails the frame and every later one, as natively.
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';

let context: zgal.Context;
let scratch: string;

before(async () => {
  scratch = mkdtempSync(join(tmpdir(), 'zgal-streaming-'));
  context = await createNodeContext([scratch]);
});

after(() => {
  context.destroy();
  rmSync(scratch, { recursive: true });
});

// A stream whose source's chunks the test enqueues, ends or fails, and whether it was cancelled
function scripted(): { stream: zgal.Stream; source: ReadableStreamDefaultController<Uint8Array>; cancelled: () => boolean } {
  let source!: ReadableStreamDefaultController<Uint8Array>;
  const readable = new ReadableStream<Uint8Array>({ start(controller) { source = controller; } });
  const { stream, cancelled } = spied(zgal.readableStream(readable, 'scripted'));
  return { stream, source, cancelled };
}

// stream, noting whether it was cancelled
function spied(inner: zgal.Stream): { stream: zgal.Stream; cancelled: () => boolean } {
  let cancelled = false;
  const stream: zgal.Stream = {
    read: (maxBytes) => inner.read(maxBytes),
    ready: (minBytes) => inner.ready(minBytes),
    cancel: () => {
      cancelled = true;
      inner.cancel();
    },
  };
  return { stream, cancelled: () => cancelled };
}

// Bytes from start, counting up
function bytes(start: number, count: number): Uint8Array<ArrayBuffer> {
  return Uint8Array.from({ length: count }, (_, i) => (start + i) & 0xff);
}

// The uints of little-endian bytes
function words(data: Uint8Array): number[] {
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  return Array.from({ length: data.length / 4 }, (_, i) => view.getUint32(i * 4, true));
}

// A graph streaming into a buffer of count words, with a readback of it
function streaming(stream: zgal.Stream, count: number, bytesPerFrame: number) {
  const graph = new zgal.RenderGraph(context);
  const buffer = graph.createStreamBuffer('scene', stream, count, bytesPerFrame);
  const read = graph.add(new zgal.BufferReadbackNode('readScene', graph, buffer));
  const contents = () => graph.node<zgal.BufferReadbackNode>(read).contents<number>();
  return { graph, buffer, contents };
}

test('frames write the whole words that have arrived, holding back a partial word, then the stream ends', async () => {
  const { stream, source } = scripted();
  const { graph, buffer, contents } = streaming(stream, 10, 16);
  assert.equal(graph.streamTotal(buffer), 40);
  assert.equal(graph.streamArrived(buffer), 0);
  // 7 bytes: one word, 3 bytes waiting for the rest of theirs
  source.enqueue(bytes(0, 7));
  await stream.ready(7);
  await graph.execute();
  assert.equal(graph.streamArrived(buffer), 4);
  // 13 more: the waiting 3 complete a word, 16 in all, the frame's limit
  source.enqueue(bytes(7, 13));
  await stream.ready(13);
  await graph.execute();
  assert.equal(graph.streamArrived(buffer), 20);
  // The rest in two chunks and the end: 16 bytes a frame, then the last 4
  source.enqueue(bytes(20, 9));
  source.enqueue(bytes(29, 11));
  source.close();
  await stream.ready(20);
  await graph.execute();
  assert.equal(graph.streamArrived(buffer), 36);
  await graph.execute();
  assert.equal(graph.streamArrived(buffer), 40);
  assert.deepEqual(await contents(), words(bytes(0, 40)));
  // The next frame sees the end
  await graph.execute();
  assert.equal(graph.streamArrived(buffer), 40);
  graph.destroy();
});

test('a frame writes at most bytesPerFrame, and nothing while nothing has arrived', async () => {
  const data = bytes(100, 40);
  const stream = zgal.readableStream(new Blob([data]).stream(), 'blob');
  await stream.ready(40);
  const { graph, buffer, contents } = streaming(stream, 10, 12);
  const seen: number[] = [];
  for (let frame = 0; frame < 5; frame++) {
    await graph.execute();
    seen.push(graph.streamArrived(buffer));
  }
  assert.deepEqual(seen, [12, 24, 36, 40, 40]);
  assert.deepEqual(await contents(), words(data));
  graph.destroy();

  const idle = scripted();
  const waiting = streaming(idle.stream, 2, 8);
  await waiting.graph.execute();
  await waiting.graph.execute();
  assert.equal(waiting.graph.streamArrived(waiting.buffer), 0);
  waiting.graph.destroy();
});

test('readStreams reads once a frame, and tells the coming frame\'s bytes before it runs', async () => {
  const { stream, source } = scripted();
  const { graph, buffer, contents } = streaming(stream, 4, 16);
  source.enqueue(bytes(0, 4));
  await stream.ready(4);
  graph.readStreams();
  assert.equal(graph.streamArrived(buffer), 4);
  // More bytes arriving before the frame wait for the next one
  source.enqueue(bytes(4, 4));
  await stream.ready(4);
  graph.readStreams();
  assert.equal(graph.streamArrived(buffer), 4);
  await graph.execute();
  assert.equal(graph.streamArrived(buffer), 4);
  graph.readStreams();
  assert.equal(graph.streamArrived(buffer), 8);
  await graph.execute();
  assert.deepEqual((await contents()).slice(0, 2), words(bytes(0, 8)));
  graph.destroy();
});

test('a graph without nodes still streams', async () => {
  const stream = zgal.readableStream(new Blob([bytes(0, 16)]).stream(), 'blob');
  await stream.ready(16);
  const graph = new zgal.RenderGraph(context);
  const buffer = graph.createStreamBuffer('scene', stream, 4, 8);
  await graph.execute();
  await graph.execute();
  assert.equal(graph.streamArrived(buffer), 16);
  graph.destroy();
});

test('a frame failing after its writes keeps them; one failing before them writes them later', async () => {
  const data = bytes(0, 40);
  const stream = zgal.readableStream(new Blob([data]).stream(), 'blob');
  await stream.ready(40);
  const { graph, buffer, contents } = streaming(stream, 10, 8);
  let failRecord = true;
  let failCount = false;
  graph.add({
    name: 'failing',
    record: () => {
      if (failRecord) throw new Error('recording failed');
    },
  });
  graph.createBuffer('sized', zgal.uint, () => {
    if (failCount) throw new Error('sizing failed');
    return 1;
  });
  // Recording comes after the frame's bytes are written: they count, and the next frame goes on
  await assert.rejects(graph.execute(), /recording failed/);
  assert.equal(graph.streamArrived(buffer), 8);
  await assert.rejects(graph.execute(), /recording failed/);
  assert.equal(graph.streamArrived(buffer), 16);
  // Sizing the buffers comes before the writes: the frame's bytes wait for a frame that writes them
  failRecord = false;
  failCount = true;
  await assert.rejects(graph.execute(), /sizing failed/);
  assert.equal(graph.streamArrived(buffer), 24);
  await assert.rejects(graph.execute(), /sizing failed/);
  assert.equal(graph.streamArrived(buffer), 24);
  failCount = false;
  for (let frame = 0; frame < 3; frame++) await graph.execute();
  assert.equal(graph.streamArrived(buffer), 40);
  assert.deepEqual(await contents(), words(data));
  graph.destroy();
});

test('a failing source fails the frame and every later one, and is cancelled', async () => {
  const { stream, source, cancelled } = scripted();
  const { graph, buffer, contents } = streaming(stream, 4, 16);
  source.enqueue(bytes(0, 4));
  await stream.ready(4);
  await graph.execute();
  await contents();
  source.error(new Error('the network went away'));
  await assert.rejects(stream.ready(1));
  await assert.rejects(graph.execute(), /^Error: streaming buffer 'scene': the source failed: scripted: the network went away$/);
  assert.equal(cancelled(), true);
  await assert.rejects(graph.execute(), /streaming buffer 'scene': the source failed/);
  assert.throws(() => graph.readStreams(), /streaming buffer 'scene': the source failed/);
  // What the earlier frame wrote stays
  assert.equal(graph.streamArrived(buffer), 4);
  graph.destroy();
});

test('a stream giving more than the buffer holds, or ending early, fails', async () => {
  // 9 bytes for 8: the frame reads one byte more than the size, to see the end
  const more = spied(zgal.readableStream(new Blob([bytes(0, 12)]).stream(), 'blob'));
  await more.stream.ready(12);
  const larger = streaming(more.stream, 2, 16);
  await assert.rejects(larger.graph.execute(), /^Error: streaming buffer 'scene': the stream holds more than the buffer's 8 bytes$/);
  assert.equal(more.cancelled(), true);
  larger.graph.destroy();

  // Ending within the third word: two whole words are written, then the end fails
  const early = zgal.readableStream(new Blob([bytes(0, 10)]).stream(), 'blob');
  // Resolves at the end
  await early.ready(11);
  const shorter = streaming(early, 4, 16);
  await shorter.graph.execute();
  assert.equal(shorter.graph.streamArrived(shorter.buffer), 8);
  await assert.rejects(shorter.graph.execute(), /^Error: streaming buffer 'scene': the stream ended after 10 of the buffer's 16 bytes$/);
  // (Destroying a graph while its readback maps rejects the map unhandled)
  await shorter.contents();
  shorter.graph.destroy();

  // A stream reading more than it was asked for
  const greedy: zgal.Stream = { read: () => bytes(0, 32), ready: async () => {}, cancel: () => {} };
  const broken = streaming(greedy, 8, 16);
  await assert.rejects(broken.graph.execute(), /^Error: streaming buffer 'scene': the source read 32 bytes into room for 16$/);
  broken.graph.destroy();
});

test('stream buffers check their sizes, and only they stream', async () => {
  const graph = new zgal.RenderGraph(context);
  const { stream } = scripted();
  for (const perFrame of [0, 6, -4, 4.5]) {
    assert.throws(() => graph.createStreamBuffer('scene', stream, 4, perFrame), {
      name: 'RangeError',
      message: `streamed buffer 'scene' appends ${perFrame} bytes per frame, which isn't a positive multiple of 4`,
    });
  }
  assert.throws(() => graph.createStreamBuffer('scene', stream, 1.5, 4), RangeError);
  const plain = graph.createBuffer('plain', zgal.uint, 4);
  assert.throws(() => graph.streamArrived(plain), /buffer 'plain' isn't streamed/);
  assert.throws(() => graph.streamTotal(plain), /buffer 'plain' isn't streamed/);
  // More than the device's buffers hold fails the first frame
  const huge = scripted();
  const words = Math.floor(context.device.limits.maxBufferSize / 4) + 1;
  const tooLarge = graph.createStreamBuffer('huge', huge.stream, words, 64);
  await assert.rejects(graph.execute(), /streamed buffer 'huge' holds \d+ bytes, more than the device's buffers can/);
  assert.equal(graph.streamTotal(tooLarge), words * 4);
  graph.destroy();
});

test('destroying a graph cancels its streams', async () => {
  const { stream, source, cancelled } = scripted();
  const { graph, contents } = streaming(stream, 4, 16);
  source.enqueue(bytes(0, 4));
  await stream.ready(4);
  await graph.execute();
  await contents();
  graph.destroy();
  assert.equal(cancelled(), true);
  assert.throws(() => stream.read(4), /scripted: cancelled/);
});
