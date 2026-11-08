// Streams for stream-buffers: bytes a program reads over frames (e.g. a scene file) instead of
// loading them whole when its graph is built. A program opens one through a StreamOpener, usually
// context.loadAssetStream, or wraps any ReadableStream of bytes with readableStream.

import type { Context } from './context.ts';
import { decode, stride, type Type } from './types.ts';

// Bytes with no known length, read as they arrive. A Stream is an object, so every reference to one
// shares its read position (as copies of a native zgal::Stream do), and, like the native one, it
// isn't meant for more than one reader.
export interface Stream {
  // Takes up to maxBytes (a whole number) of the bytes that have arrived, without waiting: an empty
  // array when none are waiting yet, null once the stream has ended and every byte was read. The
  // array may share memory with the source's chunks; read it before the next read. Throws the
  // source's failure once it has failed, even with bytes from before the failure waiting.
  read(maxBytes: number): Uint8Array<ArrayBuffer> | null;
  // Resolves once at least minBytes (a whole number) are waiting to be read, or the stream has
  // ended with fewer; rejects with the source's failure (failed already, or while waiting).
  ready(minBytes: number): Promise<void>;
  // Stops the stream for a reader done with it before its end (a graph destroyed mid-stream, e.g.
  // when a page stops): drops the bytes waiting, stops reading the source (cancelling a
  // ReadableStream's, unless it has ended), rejects the waits, and makes later reads throw. Does
  // nothing once cancelled.
  cancel(): void;
}

// What a program supplies for a stream input: opens the stream at a path. Failing to open (e.g. a
// missing file) rejects, which fails the graph's build; a source failing later fails reads.
export type StreamOpener = (path: string, context: Context) => Promise<Stream>;

function checkCount(count: number, what: string): void {
  if (!Number.isInteger(count) || count < 0) throw new RangeError(`a stream's ${what} must be a whole number, not ${count}`);
}

// A source's bytes, which arrive in chunks, as a Stream. It reads the source as fast as the source
// gives bytes, keeping them until they're read (a stream-buffer's frames take at most #:per-frame
// bytes each, and its buffer holds the whole stream anyway). name prefixes its failures' messages.
class ReadableSourceStream implements Stream {
  // The chunks not yet read whole start at chunks[head] (those before it are read and dropped)
  private chunks: Uint8Array<ArrayBuffer>[] = [];
  private head = 0;
  // Bytes already read from chunks[head]
  private offset = 0;
  // Bytes arrived and not yet read
  private waiting = 0;
  private ended = false;
  private failure: Error | null = null;
  private cancelled = false;
  private readonly waiters: { minBytes: number; resolve: () => void; reject: (error: Error) => void }[] = [];
  private readonly name: string;
  private readonly reader: ReadableStreamDefaultReader<Uint8Array>;

  constructor(source: ReadableStream<Uint8Array>, name: string) {
    this.name = name;
    this.reader = source.getReader();
    void this.pump(this.reader);
  }

  private async pump(reader: ReadableStreamDefaultReader<Uint8Array>): Promise<void> {
    try {
      for (;;) {
        const { done, value } = await reader.read();
        // Cancelling settled everything already
        if (this.cancelled) return;
        if (done) break;
        if (!(value instanceof Uint8Array)) throw new TypeError(`gave a chunk that isn't a Uint8Array (${Object.prototype.toString.call(value)})`);
        if (value.length === 0) continue;
        // Bytes over an ArrayBuffer (a SharedArrayBuffer's are copied)
        this.chunks.push(value.buffer instanceof ArrayBuffer ? value as Uint8Array<ArrayBuffer> : new Uint8Array(value));
        this.waiting += value.length;
        this.settle();
      }
      this.ended = true;
    } catch (error) {
      if (this.cancelled) return;
      this.failure = new Error(`${this.name}: ${error instanceof Error ? error.message : String(error)}`, { cause: error });
      reader.cancel(error).catch(() => {});
    }
    this.settle();
  }

  // Resolves or rejects the waiters whose wait is over
  private settle(): void {
    for (let i = 0; i < this.waiters.length;) {
      const waiter = this.waiters[i];
      if (this.failure) {
        waiter.reject(this.failure);
      } else if (this.ended || this.waiting >= waiter.minBytes) {
        waiter.resolve();
      } else {
        i++;
        continue;
      }
      this.waiters.splice(i, 1);
    }
  }

  read(maxBytes: number): Uint8Array<ArrayBuffer> | null {
    checkCount(maxBytes, 'read size');
    if (this.failure) throw this.failure;
    if (this.waiting === 0) return this.ended ? null : new Uint8Array(0);
    const count = Math.min(maxBytes, this.waiting);
    const first = this.chunks[this.head];
    let bytes: Uint8Array<ArrayBuffer>;
    if (first.length - this.offset >= count) {
      // Within the first chunk: no copy
      bytes = first.subarray(this.offset, this.offset + count);
    } else {
      bytes = new Uint8Array(count);
      let at = 0;
      for (let i = 0; at < count; i++) {
        const chunk = this.chunks[this.head + i];
        const start = i === 0 ? this.offset : 0;
        const part = Math.min(chunk.length - start, count - at);
        bytes.set(chunk.subarray(start, start + part), at);
        at += part;
      }
    }
    // Drop what was read
    let left = count;
    while (left > 0) {
      const rest = this.chunks[this.head].length - this.offset;
      if (rest > left) {
        this.offset += left;
        break;
      }
      left -= rest;
      this.head++;
      this.offset = 0;
    }
    // Drops the chunks read once they're at least half the array (amortized constant time per chunk)
    if (this.head * 2 >= this.chunks.length) {
      this.chunks = this.chunks.slice(this.head);
      this.head = 0;
    }
    this.waiting -= count;
    return bytes;
  }

  ready(minBytes: number): Promise<void> {
    try {
      checkCount(minBytes, 'ready size');
    } catch (error) {
      return Promise.reject(error);
    }
    return new Promise((resolve, reject) => {
      this.waiters.push({ minBytes, resolve, reject });
      this.settle();
    });
  }

  cancel(): void {
    if (this.cancelled) return;
    this.cancelled = true;
    this.chunks = [];
    this.head = 0;
    this.offset = 0;
    this.waiting = 0;
    this.failure ??= new Error(`${this.name}: cancelled`);
    this.settle();
    // A source that has ended has nothing left to cancel
    if (!this.ended) this.reader.cancel().catch(() => {});
  }
}

// A ReadableStream of bytes (a fetch response's body, a Blob's stream(), a Node file's, or one a
// program makes) as a Stream, whose failures' messages start with name. The source's chunks must be
// Uint8Arrays; anything else fails the stream.
export function readableStream(source: ReadableStream<Uint8Array>, name: string): Stream {
  return new ReadableSourceStream(source, name);
}

// A stream's first count bytes (a whole number; fewer if it ends first), read without losing them:
// head holds them, and stream gives them again and then the rest. Rejects with the stream's
// failure. For an opener that checks the start of a stream (e.g. a header's magic) before handing
// it on to a stream-buffer, which then reads it whole.
export async function peekStream(stream: Stream, count: number): Promise<{ head: Uint8Array<ArrayBuffer>; stream: Stream }> {
  checkCount(count, 'peek size');
  const head = new Uint8Array(count);
  let filled = 0;
  while (filled < count) {
    await stream.ready(count - filled);
    const bytes = stream.read(count - filled);
    if (bytes === null) break;
    head.set(bytes, filled);
    filled += bytes.length;
  }
  return { head: head.subarray(0, filled), stream: new ReplayStream(head.subarray(0, filled), stream) };
}

// head's bytes, then inner's
class ReplayStream implements Stream {
  private head: Uint8Array<ArrayBuffer>;
  private served = 0;
  private readonly inner: Stream;

  constructor(head: Uint8Array<ArrayBuffer>, inner: Stream) {
    this.head = head;
    this.inner = inner;
  }

  read(maxBytes: number): Uint8Array<ArrayBuffer> | null {
    checkCount(maxBytes, 'read size');
    const rest = this.head.length - this.served;
    if (rest === 0) return this.inner.read(maxBytes);
    // The head, then what the inner stream has, up to maxBytes
    const count = Math.min(maxBytes, rest);
    const start = this.head.subarray(this.served, this.served + count);
    this.served += count;
    if (count === maxBytes) return start;
    let more: Uint8Array | null;
    try {
      more = this.inner.read(maxBytes - count);
    } catch {
      // The inner stream's failure stays: the next read throws it
      return start;
    }
    if (more === null || more.length === 0) return start;
    const both = new Uint8Array(count + more.length);
    both.set(start);
    both.set(more, count);
    return both;
  }

  ready(minBytes: number): Promise<void> {
    try {
      checkCount(minBytes, 'ready size');
    } catch (error) {
      return Promise.reject(error);
    }
    const rest = this.head.length - this.served;
    return rest >= minBytes ? Promise.resolve() : this.inner.ready(minBytes - rest);
  }

  cancel(): void {
    this.head = new Uint8Array(0);
    this.served = 0;
    this.inner.cancel();
  }
}

// A stream that has failed with failure
class FailedStream implements Stream {
  private readonly failure: Error;
  constructor(failure: Error) {
    this.failure = failure;
  }
  read(_maxBytes: number): Uint8Array<ArrayBuffer> | null {
    throw this.failure;
  }
  ready(_minBytes: number): Promise<void> {
    return Promise.reject(this.failure);
  }
  cancel(): void {}
}

function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

// What a graph's build does for a stream-buffer (as native zgal::openStream): opens path with open
// (unresolved: the opener resolves it, as context.loadAssetStream does against the assets) and
// reads header's bytes, which stream gives again before the rest, so the buffer holds the whole
// stream. Failing to open rejects, with what (e.g. "loading G's stream-buffer b") starting the
// message. A short header, or the stream failing while it's read, leaves header zeroed and stream
// failing on its first read, so the graph fails its first frame, as for any other failure of the
// stream.
export async function openStream<H>(open: StreamOpener, path: string, context: Context, header: Type<H>, what: string): Promise<{ header: H; stream: Stream }> {
  let inner: Stream;
  try {
    inner = await open(path, context);
  } catch (error) {
    throw new Error(`${what}: opening stream '${path}': ${messageOf(error)}`, { cause: error });
  }
  const size = stride(header);
  const failed = (failure: Error) => {
    inner.cancel();
    return { header: decode(header, new Uint8Array(size), 1)[0], stream: new FailedStream(failure) };
  };
  let peeked: { head: Uint8Array<ArrayBuffer>; stream: Stream };
  try {
    peeked = await peekStream(inner, size);
  } catch (error) {
    return failed(new Error(`reading the header of stream '${path}': ${messageOf(error)}`, { cause: error }));
  }
  if (peeked.head.length < size) return failed(new Error(`stream '${path}' ended after ${peeked.head.length} bytes of its ${size}-byte header`));
  return { header: decode(header, peeked.head, 1)[0], stream: peeked.stream };
}
