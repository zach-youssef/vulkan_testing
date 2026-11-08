// Streams for stream-buffers: bytes a program reads over frames (e.g. a scene file) instead of
// loading them whole when its graph is built. A program opens one through a StreamOpener, usually
// context.loadAssetStream, or wraps any ReadableStream of bytes with readableStream.

import type { Context } from './context.ts';

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
  private readonly waiters: { minBytes: number; resolve: () => void; reject: (error: Error) => void }[] = [];
  private readonly name: string;

  constructor(source: ReadableStream<Uint8Array>, name: string) {
    this.name = name;
    void this.pump(source.getReader());
  }

  private async pump(reader: ReadableStreamDefaultReader<Uint8Array>): Promise<void> {
    try {
      for (;;) {
        const { done, value } = await reader.read();
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
}

// A ReadableStream of bytes (a fetch response's body, a Blob's stream(), a Node file's, or one a
// program makes) as a Stream, whose failures' messages start with name. The source's chunks must be
// Uint8Arrays; anything else fails the stream.
export function readableStream(source: ReadableStream<Uint8Array>, name: string): Stream {
  return new ReadableSourceStream(source, name);
}
