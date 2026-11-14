// Ending a program ends its downloads, without waiting for them: a context's loads end when its
// signal is aborted (or it's destroyed), whatever its loaders do, and runHeadless (which mountHeadless
// runs) stopped while a download stalls settles at once, aborting the fetch, as does the program
// quitting mid-stream. Runs tests/stream/streamer.zgal's main against a local server whose scene
// stalls part way. web/tests/stop-streams.html checks mounted programs (stop() and Escape) in
// browsers.
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import { createServer, type Server, type ServerResponse } from 'node:http';
import type { AddressInfo } from 'node:net';
import { create, globals } from 'webgpu';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';
import { program } from '../streamer.zgal.main.ts';

// runHeadless finds WebGPU as a page does, in navigator.gpu (kept alive: a collected instance takes
// its devices with it)
const gpu = create([]);
Object.assign(globalThis, globals);
Object.defineProperty(globalThis.navigator, 'gpu', { value: gpu, configurable: true });

// How long ending may take: far less than any download's timeout
const bound = 5000;

// promise, or a rejection once bound has passed
function within<T>(promise: Promise<T>, what: string): Promise<T> {
  let timer: ReturnType<typeof setTimeout> | undefined;
  const late = new Promise<never>((_, reject) => {
    timer = setTimeout(() => reject(new Error(`${what} took more than ${bound} ms`)), bound);
  });
  return Promise.race([promise, late]).finally(() => clearTimeout(timer));
}

// A scene of a million words (it never finishes): its first bytes, then a stall
function sceneStart(words: number): Buffer {
  const scene = new Uint32Array(2 + words);
  scene[0] = 0x5a47;
  scene[1] = 1000000;
  for (let i = 2; i < scene.length; i++) scene[i] = i;
  return Buffer.from(scene.buffer);
}

// The server's requests for scene.bin: each gives `sent` bytes, then stalls until the request
// closes (the fetch aborted, or its body cancelled)
interface Served {
  sent: Promise<void>;
  closed: Promise<void>;
}
let server: Server;
let base: URL;
let sending: Buffer = Buffer.alloc(0);
let served: ((request: Served) => void) | null = null;
const responses = new Set<ServerResponse>();

before(async () => {
  server = createServer((request, response) => {
    let sent!: () => void;
    let closed!: () => void;
    const record: Served = { sent: new Promise((done) => { sent = done; }), closed: new Promise((done) => { closed = done; }) };
    response.on('close', closed);
    // (Under Node, command-line paths are made absolute)
    if (!request.url?.endsWith('/scene.bin')) {
      response.writeHead(404).end();
      return;
    }
    responses.add(response);
    response.on('close', () => responses.delete(response));
    response.writeHead(200, { 'Content-Type': 'application/octet-stream' });
    response.write(sending, () => sent());
    served?.(record);
  });
  await new Promise<void>((listening) => server.listen(0, '127.0.0.1', listening));
  base = new URL(`http://127.0.0.1:${(server.address() as AddressInfo).port}/`);
});

after(async () => {
  for (const response of responses) response.destroy();
  await new Promise((closed) => server.close(closed));
});

// The next request for scene.bin, sending bytes first
function serve(bytes: Buffer): Promise<Served> {
  sending = bytes;
  return new Promise((request) => { served = request; });
}

// What streamer prints (print!), for the running test
let printed: (text: string) => void = () => {};
zgal.output.line = (text) => printed(text);

test('aborting a context\'s signal ends its loads, whatever its loaders do', async () => {
  const loads = new AbortController();
  let cancelled = false;
  let given: AbortSignal | null = null;
  const context = await createNodeContext([], {
    signal: loads.signal,
    // Loaders that never finish, unless their signal is aborted
    assets: (_path, signal) => {
      given = signal;
      return new Promise(() => {});
    },
    assetStreams: async () => new ReadableStream<Uint8Array>({
      start(controller) { controller.enqueue(new Uint8Array([1, 2, 3])); },
      cancel() { cancelled = true; },
    }),
  });
  try {
    assert.equal(context.signal.aborted, false);
    const whole = context.loadAsset('stalled.bin');
    const stream = await context.loadAssetStream('stalled.bin');
    await stream.ready(3);
    const waiting = stream.ready(4);
    loads.abort();
    assert.equal(context.signal.aborted, true);
    assert.equal(given!.aborted, true);
    await within(assert.rejects(whole, /can't load asset stalled\.bin/), 'a whole load');
    await within(assert.rejects(waiting, /cancelled/), 'a stream');
    assert.equal(cancelled, true);
    assert.throws(() => stream.read(4), /cancelled/);
    // Later loads fail at once
    await assert.rejects(context.loadAsset('later.bin'), /can't load asset later\.bin/);
    await assert.rejects(context.loadAssetStream('later.bin'), /can't open asset stream later\.bin/);
  } finally {
    context.destroy();
  }
  // Destroying a context ends its loads too
  const other = await createNodeContext([]);
  other.destroy();
  assert.equal(other.signal.aborted, true);
});

test('a stream is cancelled by a signal that is already aborted, and lets go of the signal once it ends', async () => {
  const source = (onCancel: () => void) => new ReadableStream<Uint8Array>({
    start(controller) { controller.enqueue(new Uint8Array([1])); },
    cancel: onCancel,
  });
  // Aborted before the stream exists (the gap after a load resolves)
  const aborted = new AbortController();
  aborted.abort();
  let cancelled = false;
  const late = zgal.readableStream(source(() => { cancelled = true; }), 'late', aborted.signal);
  await within(assert.rejects(late.ready(2), /cancelled/), 'a late stream');
  assert.equal(cancelled, true);
  // A stream removes its abort listener once it ends, fails or is cancelled
  const listeners = (source: ReadableStream<Uint8Array>) => {
    const signal = new AbortController().signal;
    const counts = { added: 0, removed: 0 };
    const add = signal.addEventListener.bind(signal);
    const remove = signal.removeEventListener.bind(signal);
    signal.addEventListener = ((...args: Parameters<typeof add>) => { counts.added++; add(...args); }) as typeof add;
    signal.removeEventListener = ((...args: Parameters<typeof remove>) => { counts.removed++; remove(...args); }) as typeof remove;
    return { stream: zgal.readableStream(source, 'listened', signal), counts };
  };
  const ended = listeners(new ReadableStream<Uint8Array>({
    start(controller) { controller.enqueue(new Uint8Array([1])); controller.close(); },
  }));
  await ended.stream.ready(2);
  assert.deepEqual(ended.counts, { added: 1, removed: 1 });
  const failed = listeners(new ReadableStream<Uint8Array>({
    start(controller) { controller.error(new Error('broken')); },
  }));
  await assert.rejects(failed.stream.ready(1), /broken/);
  assert.deepEqual(failed.counts, { added: 1, removed: 1 });
  const stopped = listeners(source(() => {}));
  await stopped.stream.ready(1);
  assert.deepEqual(stopped.counts, { added: 1, removed: 0 });
  stopped.stream.cancel();
  assert.deepEqual(stopped.counts, { added: 1, removed: 1 });
});

test('stopping a run while its stream stalls in its header settles at once and aborts the fetch', async () => {
  const messages: string[] = [];
  printed = (text) => messages.push(text);
  // Half the header, then a stall: the graph's build waits for the rest
  const request = serve(sceneStart(0).subarray(0, 4));
  const states: zgal.ProgramState[] = [];
  const run = zgal.runHeadless(program, base, {
    args: ['scene.bin', '--frames', '100000'],
    onMessage: (text) => messages.push(text),
    onState: (state) => states.push(state),
  });
  const { sent, closed } = await request;
  await sent;
  await within(run.stop(), 'stop()');
  assert.equal(await run.done, 'stopped', messages.join('\n'));
  assert.deepEqual(states, ['stopped']);
  assert.deepEqual(messages, []);
  await within(closed, 'closing the stalled fetch');
});

test('stopping a run while its stream stalls part way settles at once and aborts the fetch', async () => {
  const messages: string[] = [];
  let streamed!: () => void;
  const streaming = new Promise<void>((done) => { streamed = done; });
  printed = (text) => {
    messages.push(text);
    streamed();
  };
  const request = serve(sceneStart(100));
  const run = zgal.runHeadless(program, base, {
    args: ['scene.bin', '--frames', '100000000', '--per-frame', '4'],
    onMessage: (text) => messages.push(text),
  });
  const { closed } = await request;
  await streaming;
  await within(run.stop(), 'stop()');
  assert.equal(await run.done, 'stopped', messages.join('\n'));
  assert.ok(messages.every((m) => /^streamed \d+ of 4000000$/.test(m)), messages.join('\n'));
  await within(closed, 'closing the stalled fetch');
});

test('a run quitting while its stream stalls ends the fetch', async () => {
  const messages: string[] = [];
  let quit = false;
  printed = (text) => {
    messages.push(text);
    quit = true;
  };
  // The app quits once it has seen bytes stream (as Escape would make it), its stream stalled
  const quitting: typeof program = {
    ...program,
    create: async (...args) => {
      const app = await program.create(...args);
      return new Proxy(app, {
        get(target, key) {
          if (key === 'quitRequested') return () => quit || target.quitRequested();
          const value = Reflect.get(target, key, target);
          return typeof value === 'function' ? value.bind(target) : value;
        },
      });
    },
  };
  const request = serve(sceneStart(100));
  const run = zgal.runHeadless(quitting, base, {
    args: ['scene.bin', '--frames', '100000000', '--per-frame', '4'],
    onMessage: (text) => messages.push(text),
  });
  const { closed } = await request;
  assert.equal(await within(run.done, 'quitting'), 'stopped', messages.join('\n'));
  assert.ok(messages.length > 0 && messages.every((m) => /^streamed \d+ of 4000000$/.test(m)), messages.join('\n'));
  await within(closed, 'closing the stalled fetch');
});
