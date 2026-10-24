// What the page checkers (firefox.ts, chromium.ts) share: their command line, serving a directory,
// running a ZGAL page (FILE.zgal.html) until it stops, pressing keys, and screenshotting its canvas
// and comparing it with an approved PNG. Each checker only launches and drives its browser.
//
// A page reports its state in <html data-zgal="running|stopped|failed"> (web/zgal/main-browser.ts).
// Checkers exit with 0 when the page ran without errors (and matched), 1 when it failed, didn't match
// or timed out (or the browser named doesn't exist), 2 for a bad command line, 3 when they found no
// browser where they look by default (findBrowser).
import { spawn } from 'node:child_process';
import { accessSync, constants, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs';
import { createServer, type Server } from 'node:http';
import type { AddressInfo } from 'node:net';
import { tmpdir } from 'node:os';
import { delimiter, extname, join, resolve, sep } from 'node:path';
import { parseArgs, type ParseArgsConfig } from 'node:util';
import { decodePng, encodePng } from '../zgal/png.ts';

// A page in a browser, driven by a checker
export interface Browser {
  navigate(url: string): Promise<void>;
  // The JSON value of an expression evaluated in the page (awaited when it's a promise)
  evaluate(expression: string): Promise<unknown>;
  // Presses and releases a key, given as the character it types
  pressKey(key: string): Promise<void>;
  // A PNG of the element with this id, as the page shows it
  screenshot(id: string): Promise<Uint8Array>;
  close(): Promise<void>;
}

export interface Checker<O> {
  // The checker's own options (parseArgs's), and their usage lines
  options: NonNullable<ParseArgsConfig['options']>;
  usage: string;
  // Starts the browser with the parsed options, printing the page's console with log
  launch(options: O, log: (line: string) => void): Promise<Browser>;
}

const common = {
  serve: { type: 'string' },
  wait: { type: 'string' },
  timeout: { type: 'string', default: '60000' },
  keys: { type: 'string', default: '' },
  screenshot: { type: 'string' },
  compare: { type: 'string' },
  help: { type: 'boolean', default: false },
} as const;

const contentTypes: Record<string, string> = {
  '.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.json': 'application/json',
  '.wgsl': 'text/plain', '.png': 'image/png', '.css': 'text/css',
};

// Serves the files under root, on a free port
async function serve(root: string): Promise<{ server: Server; port: number }> {
  const server = createServer((request, response) => {
    const path = decodeURIComponent(new URL(request.url ?? '/', 'http://localhost').pathname);
    const file = resolve(root, `.${path}`);
    try {
      if (file !== root && !file.startsWith(root + sep)) throw new Error('outside the served directory');
      const body = readFileSync(file);
      response.writeHead(200, { 'Content-Type': contentTypes[extname(file)] ?? 'application/octet-stream' });
      response.end(body);
    } catch {
      response.writeHead(404);
      response.end();
    }
  });
  await new Promise<void>((done) => server.listen(0, '127.0.0.1', done));
  return { server, port: (server.address() as AddressInfo).port };
}

export const sleep = (ms: number) => new Promise((done) => setTimeout(done, ms));

// The canvas's pixels from a screenshot of it shown at a whole multiple of its size (the page scales
// it so), sampling each block's centre
function unscale(shot: { width: number; height: number; rgba: Uint8Array }, width: number, height: number) {
  const scale = shot.width / width;
  if (!Number.isInteger(scale) || shot.height !== height * scale) {
    throw new Error(`the screenshot (${shot.width}x${shot.height}) isn't a whole multiple of the canvas (${width}x${height})`);
  }
  const rgba = new Uint8Array(width * height * 4);
  const half = Math.floor(scale / 2);
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const from = ((y * scale + half) * shot.width + x * scale + half) * 4;
      rgba.set(shot.rgba.subarray(from, from + 4), (y * width + x) * 4);
    }
  }
  return { width, height, rgba };
}

// Parses the command line (PAGE: a URL, or with --serve a path under the served directory), runs the
// page in the checker's browser and exits
export async function runChecker<O>(name: string, checker: Checker<O>): Promise<never> {
  const usage = `usage: node web/tools/${name}.ts [--serve DIR] [--wait MS | --timeout MS] [--keys KEYS]
       [--screenshot FILE] [--compare FILE] ${checker.usage} PAGE`;
  let parsed;
  try {
    parsed = parseArgs({ allowPositionals: true, options: { ...common, ...checker.options } });
  } catch (error) {
    console.error(`${error instanceof Error ? error.message : String(error)}\n${usage}`);
    process.exit(2);
  }
  const { values, positionals } = parsed;
  const options = values as { [K in keyof typeof common]: (typeof common)[K]['type'] extends 'boolean' ? boolean : string | undefined };
  if (options.help) {
    console.log(usage);
    process.exit(0);
  }
  const milliseconds = (option: string, text: string | undefined): number | undefined => {
    if (text === undefined) return undefined;
    if (!/^\d+$/.test(text)) {
      console.error(`--${option} expects milliseconds, not '${text}'\n${usage}`);
      process.exit(2);
    }
    return Number(text);
  };
  const wait = milliseconds('wait', options.wait);
  const timeout = milliseconds('timeout', options.timeout)!;
  if (positionals.length !== 1) {
    console.error(usage);
    process.exit(2);
  }

  let failed = false;
  const fail = (message: string) => {
    console.error(message);
    failed = true;
  };

  const served = options.serve ? await serve(resolve(options.serve)) : null;
  const url = served ? `http://127.0.0.1:${served.port}/${positionals[0].replace(/^\//, '')}` : positionals[0];
  let browser: Browser | null = null;
  try {
    browser = await checker.launch(values as O, (line) => console.log(line));
    const page = browser;
    const state = async (): Promise<string> => (await page.evaluate('document.documentElement.dataset.zgal ?? ""')) as string;

    await page.navigate(url);
    const adapter = await page.evaluate(`(async () => {
      const adapter = navigator.gpu && await navigator.gpu.requestAdapter();
      // Firefox leaves the adapter's description empty
      return adapter ? \`\${adapter.info.vendor} \${adapter.info.architecture}\`.trim() || 'available' : navigator.gpu ? 'none' : 'no WebGPU';
    })()`);
    console.log(`adapter: ${adapter}`);

    // Until it runs (or fails starting), then the keys, then until it stops (or for --wait)
    const deadline = Date.now() + timeout;
    const until = async (done: (s: string) => boolean): Promise<string> => {
      for (;;) {
        const s = await state();
        if (done(s) || Date.now() > deadline) return s;
        await sleep(100);
      }
    };
    let reached = await until((s) => s !== '');
    for (const key of options.keys ?? '') {
      if (reached !== 'running') break;
      await page.pressKey(key);
      console.log(`pressed ${key}`);
    }
    if (wait !== undefined) {
      await sleep(wait);
      reached = await state();
    } else {
      reached = await until((s) => s === 'stopped' || s === 'failed');
    }
    const message = (await page.evaluate('document.getElementById("zgal-message")?.textContent ?? ""')) as string;
    if (message) console.log(`page:\n${message.trimEnd()}`);
    if (reached === 'failed') fail('the page failed');
    else if (reached === '' || (wait === undefined && reached === 'running')) {
      fail(`the page ${reached === '' ? 'didn\'t start' : 'didn\'t stop'} in ${timeout} ms`);
    }

    if (!failed && (options.screenshot || options.compare)) {
      const size = (await page.evaluate('[document.getElementById("zgal").width, document.getElementById("zgal").height]')) as [number, number];
      const pixels = unscale(decodePng(await page.screenshot('zgal')), size[0], size[1]);
      if (options.screenshot) {
        writeFileSync(options.screenshot, encodePng(pixels.width, pixels.height, pixels.rgba));
        console.log(`wrote ${options.screenshot} (${pixels.width}x${pixels.height})`);
      }
      if (options.compare) {
        const approved = decodePng(readFileSync(options.compare));
        if (approved.width !== pixels.width || approved.height !== pixels.height) {
          fail(`the canvas is ${pixels.width}x${pixels.height}, ${options.compare} ${approved.width}x${approved.height}`);
        } else {
          let differing = 0;
          for (let i = 0; i < pixels.width * pixels.height; i++) {
            for (let c = 0; c < 4; c++) {
              if (pixels.rgba[i * 4 + c] !== approved.rgba[i * 4 + c]) {
                differing++;
                break;
              }
            }
          }
          if (differing > 0) fail(`${differing} pixels differ from ${options.compare}`);
          else console.log(`matches ${options.compare}`);
        }
      }
    }
  } catch (error) {
    fail(error instanceof Error ? error.message : String(error));
  } finally {
    await browser?.close();
    served?.server.close();
  }
  process.exit(failed ? 1 : 0);
}

// The value of a script evaluated as JSON in the page (both drivers return strings as they are)
export function jsonExpression(expression: string): string {
  return `(async () => JSON.stringify((await (${expression})) ?? null))()`;
}

// The program a path names, or a name on PATH names, if it exists
function executable(program: string): string | null {
  const runnable = (path: string) => {
    try {
      accessSync(path, constants.X_OK);
      return statSync(path).isFile();
    } catch {
      return false;
    }
  };
  if (program.includes(sep)) return runnable(program) ? program : null;
  for (const directory of (process.env.PATH ?? '').split(delimiter)) {
    if (directory && runnable(join(directory, program))) return join(directory, program);
  }
  return null;
}

// The browser's binary: the one named (by an option or an environment variable), exiting with 1 when
// it doesn't exist, as whoever named it expects it to run; else the first of the defaults (paths, or
// names on PATH) that exists, exiting with 3 (no browser, so the caller can skip it) when none does.
// An empty name (e.g. CHROMIUM=) names nothing.
export function findBrowser(name: string, named: string | undefined, defaults: string[], hint: string): string {
  if (named !== undefined && named !== '') {
    const found = executable(named);
    if (!found) {
      console.error(`No ${name} at ${named}`);
      process.exit(1);
    }
    return found;
  }
  for (const candidate of defaults) {
    const found = executable(candidate);
    if (found) return found;
  }
  console.error(`No ${name}${defaults.length > 0 ? ` (looked for ${defaults.join(', ')})` : ''}: ${hint}`);
  process.exit(3);
}

// Starts a browser in a fresh profile directory (deleted when it stops), waiting for its stderr to
// match listening (the address its debugging protocol listens on)
export async function startBrowser(name: string, binary: string, args: (profile: string) => string[], listening: RegExp):
  Promise<{ address: string; stop(): Promise<void> }> {
  const profile = mkdtempSync(join(tmpdir(), `zgal-${name.toLowerCase()}-`));
  const child = spawn(binary, args(profile), { stdio: ['ignore', 'ignore', 'pipe'] });
  const exited = new Promise<void>((done) => child.on('exit', () => done()));
  const stop = async () => {
    child.kill();
    await Promise.race([exited, sleep(5000)]);
    if (child.exitCode === null && child.signalCode === null) child.kill('SIGKILL');
    rmSync(profile, { recursive: true, force: true });
  };
  let stderr = '';
  const address = new Promise<string>((done, reject) => {
    child.stderr.on('data', (chunk) => {
      stderr += chunk;
      const match = listening.exec(stderr);
      if (match) done(match[1]);
    });
    child.on('error', reject);
    child.on('exit', () => reject(new Error(`${name} exited before listening:\n${stderr}`)));
  });
  try {
    return { address: await Promise.race([address, sleep(30000).then(() => { throw new Error(`${name} didn't start listening in 30 s`); })]), stop };
  } catch (error) {
    await stop();
    throw error;
  }
}

// A WebSocket's JSON messages: replies by id, events to onEvent
export class JsonSocket {
  private next = 0;
  private pending = new Map<number, (reply: any) => void>();
  private readonly socket: WebSocket;

  private constructor(socket: WebSocket, onEvent: (message: any) => void) {
    this.socket = socket;
    socket.onmessage = (message) => {
      const data = JSON.parse(String(message.data));
      if (data.id !== undefined && this.pending.has(data.id)) {
        this.pending.get(data.id)!(data);
        this.pending.delete(data.id);
      } else {
        onEvent(data);
      }
    };
  }

  static async connect(url: string, onEvent: (message: any) => void): Promise<JsonSocket> {
    const socket = new WebSocket(url);
    await new Promise((done, reject) => {
      socket.onopen = done;
      socket.onerror = () => reject(new Error(`couldn't connect to ${url}`));
    });
    return new JsonSocket(socket, onEvent);
  }

  // The reply to a message (given its fields besides id)
  request(message: object): Promise<any> {
    const id = ++this.next;
    return new Promise((done) => {
      this.pending.set(id, done);
      this.socket.send(JSON.stringify({ id, ...message }));
    });
  }

  close(): void {
    this.socket.close();
  }
}
