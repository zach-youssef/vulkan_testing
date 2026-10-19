// Runs a ZGAL page (FILE.zgal.html) in Firefox and checks it: prints the page's console and messages,
// optionally presses keys, screenshots its canvas and compares it with an approved PNG. Exits with 0
// when the page ran without errors (and matched), 1 when it failed, didn't match or timed out, 2 for a
// bad command line.
//
//     node web/tools/firefox.ts [options] PAGE
//
//   PAGE                a URL, or with --serve a path under the served directory (with its query
//                       string, e.g. 'smb3.zgal.html?frames=120')
//   --serve DIR         serves DIR over HTTP while the page runs (modules don't load from file: URLs)
//   --wait MS           waits MS milliseconds (for pages that run until closed), rather than until the
//                       page stops (its app quits or runs its ?frames=N), fails, or --timeout passes
//   --timeout MS        how long to wait for the page to stop (default 60000)
//   --keys KEYS         presses each character of KEYS once the page runs (e.g. '3' or 'ab')
//   --screenshot FILE   writes the canvas's pixels (at its own resolution, not the page's scale)
//   --compare FILE      compares the canvas with an approved PNG of the same size
//   --headless          runs without a window (Firefox has no WebGPU adapter headless, so pages fail)
//   --firefox PATH      the Firefox binary (default $FIREFOX, else the macOS application's)
//
// Firefox is driven with WebDriver BiDi in a fresh profile, deleted afterwards. The page reports its
// state in <html data-zgal="running|stopped|failed"> (web/zgal/main-browser.ts).
import { spawn } from 'node:child_process';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { createServer, type Server } from 'node:http';
import type { AddressInfo } from 'node:net';
import { tmpdir } from 'node:os';
import { extname, join, resolve, sep } from 'node:path';
import { parseArgs } from 'node:util';
import { decodePng, encodePng } from '../zgal/png.ts';

const usage = `usage: node web/tools/firefox.ts [--serve DIR] [--wait MS | --timeout MS] [--keys KEYS]
       [--screenshot FILE] [--compare FILE] [--headless] [--firefox PATH] PAGE`;

let parsed;
try {
  parsed = parseArgs({
    allowPositionals: true,
    options: {
      serve: { type: 'string' },
      wait: { type: 'string' },
      timeout: { type: 'string', default: '60000' },
      keys: { type: 'string', default: '' },
      screenshot: { type: 'string' },
      compare: { type: 'string' },
      headless: { type: 'boolean', default: false },
      firefox: { type: 'string', default: process.env.FIREFOX ?? '/Applications/Firefox.app/Contents/MacOS/firefox' },
      help: { type: 'boolean', default: false },
    },
  });
} catch (error) {
  console.error(`${error instanceof Error ? error.message : String(error)}\n${usage}`);
  process.exit(2);
}
const { values: options, positionals } = parsed;
if (options.help) {
  console.log(usage);
  process.exit(0);
}
const milliseconds = (name: string, text: string | undefined): number | undefined => {
  if (text === undefined) return undefined;
  if (!/^\d+$/.test(text)) {
    console.error(`--${name} expects milliseconds, not '${text}'\n${usage}`);
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

const sleep = (ms: number) => new Promise((done) => setTimeout(done, ms));

type Reply = { type: 'success'; result: any } | { type: 'error'; error: string; message: string };

// A WebDriver BiDi session over a WebSocket
class Bidi {
  private next = 0;
  private pending = new Map<number, (reply: Reply) => void>();
  private socket: WebSocket;

  constructor(socket: WebSocket, onEvent: (method: string, params: any) => void) {
    this.socket = socket;
    socket.onmessage = (message) => {
      const data = JSON.parse(String(message.data));
      if (data.id !== undefined && this.pending.has(data.id)) {
        this.pending.get(data.id)!(data);
        this.pending.delete(data.id);
      } else if (data.method) {
        onEvent(data.method, data.params);
      }
    };
  }

  async send(method: string, params: object): Promise<any> {
    const id = ++this.next;
    const reply = await new Promise<Reply>((done) => {
      this.pending.set(id, done);
      this.socket.send(JSON.stringify({ id, method, params }));
    });
    if (reply.type === 'error') throw new Error(`${method}: ${reply.error}: ${reply.message}`);
    return reply.result;
  }

  close(): void {
    this.socket.close();
  }
}

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

let failed = false;
const fail = (message: string) => {
  console.error(message);
  failed = true;
};

const served = options.serve ? await serve(resolve(options.serve)) : null;
const url = served ? `http://127.0.0.1:${served.port}/${positionals[0].replace(/^\//, '')}` : positionals[0];
const profile = mkdtempSync(join(tmpdir(), 'zgal-firefox-'));
writeFileSync(join(profile, 'user.js'), [
  'user_pref("dom.webgpu.enabled", true);',
  'user_pref("browser.shell.checkDefaultBrowser", false);',
  'user_pref("datareporting.policy.dataSubmissionEnabled", false);',
  'user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);',
].join('\n'));
// Port 0: Firefox picks a free one and prints it. A fresh profile and instance, so a Firefox already
// open isn't touched; the window is large enough to show a 256x240 canvas at 3x.
const firefox = spawn(options.firefox, ['--remote-debugging-port', '0', '--profile', profile, '--no-remote',
  '--new-instance', '--window-size', '1024,900', ...(options.headless ? ['--headless'] : [])],
  { stdio: ['ignore', 'ignore', 'pipe'] });
const exited = new Promise<void>((done) => firefox.on('exit', () => done()));
let stderr = '';
const endpoint = new Promise<string>((done, reject) => {
  firefox.stderr.on('data', (chunk) => {
    stderr += chunk;
    const match = /WebDriver BiDi listening on (ws:\/\/\S+)/.exec(stderr);
    if (match) done(match[1]);
  });
  firefox.on('error', reject);
  firefox.on('exit', () => reject(new Error(`Firefox exited before listening:\n${stderr}`)));
});

let bidi: Bidi | null = null;
try {
  const address = await Promise.race([endpoint, sleep(30000).then(() => { throw new Error('Firefox didn\'t start listening in 30 s'); })]);
  const socket = new WebSocket(`${address}/session`);
  await new Promise((done, reject) => {
    socket.onopen = done;
    socket.onerror = () => reject(new Error(`couldn't connect to ${address}`));
  });
  bidi = new Bidi(socket, (method, params) => {
    if (method === 'log.entryAdded') console.log(`[${params.level}] ${params.text}`);
  });
  await bidi.send('session.new', { capabilities: {} });
  await bidi.send('session.subscribe', { events: ['log.entryAdded'] });
  const context = (await bidi.send('browsingContext.getTree', {})).contexts[0].context;
  const evaluate = async (expression: string): Promise<any> =>
    (await bidi!.send('script.evaluate', { target: { context }, expression, awaitPromise: true })).result;
  const state = async (): Promise<string> => (await evaluate('document.documentElement.dataset.zgal ?? ""')).value;

  await bidi.send('browsingContext.navigate', { context, url, wait: 'complete' });
  const adapter = await evaluate(`(async () => {
    const adapter = navigator.gpu && await navigator.gpu.requestAdapter();
    // Firefox leaves the adapter's description empty
    return adapter ? \`\${adapter.info.vendor} \${adapter.info.architecture}\`.trim() || 'available' : navigator.gpu ? 'none' : 'no WebGPU';
  })()`);
  console.log(`adapter: ${adapter.value}`);

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
  for (const key of options.keys) {
    if (reached !== 'running') break;
    await bidi.send('input.performActions', { context, actions: [{ type: 'key', id: 'keyboard',
      actions: [{ type: 'keyDown', value: key }, { type: 'pause', duration: 50 }, { type: 'keyUp', value: key }] }] });
    console.log(`pressed ${key}`);
  }
  if (wait !== undefined) {
    await sleep(wait);
    reached = await state();
  } else {
    reached = await until((s) => s === 'stopped' || s === 'failed');
  }
  const message = (await evaluate('document.getElementById("zgal-message")?.textContent ?? ""')).value as string;
  if (message) console.log(`page:\n${message.trimEnd()}`);
  if (reached === 'failed') fail('the page failed');
  else if (reached === '' || (wait === undefined && reached === 'running')) {
    fail(`the page ${reached === '' ? 'didn\'t start' : 'didn\'t stop'} in ${timeout} ms`);
  }

  if (!failed && (options.screenshot || options.compare)) {
    const canvas = await evaluate('document.getElementById("zgal")');
    const size = (await evaluate('[document.getElementById("zgal").width, document.getElementById("zgal").height]'))
      .value.map((v: { value: number }) => v.value) as [number, number];
    const shot = await bidi.send('browsingContext.captureScreenshot',
      { context, clip: { type: 'element', element: { sharedId: canvas.sharedId } } });
    const pixels = unscale(decodePng(Buffer.from(shot.data, 'base64')), size[0], size[1]);
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
  bidi?.close();
  firefox.kill();
  await Promise.race([exited, sleep(5000)]);
  if (firefox.exitCode === null && firefox.signalCode === null) firefox.kill('SIGKILL');
  served?.server.close();
  rmSync(profile, { recursive: true, force: true });
}
process.exit(failed ? 1 : 0);
