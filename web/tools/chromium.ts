// Runs a ZGAL page (FILE.zgal.html) in headless Chromium and checks it, as firefox.ts does in Firefox:
// prints the page's console and messages, optionally presses keys, screenshots its canvas and compares
// it with an approved PNG. Exits with 0 when the page ran without errors (and matched), 1 when it
// failed, didn't match or timed out, 2 for a bad command line, 3 when no Chromium was found.
//
//     node web/tools/chromium.ts [options] PAGE
//
//   PAGE, --serve, --wait, --timeout, --keys, --screenshot, --compare: as firefox.ts's
//   --hardware          uses the machine's GPU rather than SwiftShader
//   --chromium PATH     the Chromium binary (default $CHROMIUM, else the newest headless shell
//                       Playwright installed: npx playwright install chromium-headless-shell)
//
// Pages run on SwiftShader, Chromium's software WebGPU adapter, by default, so they run without a GPU
// or a window. Chromium's compositor runs on SwiftShader too (--use-angle=swiftshader), as it does
// where the GPU is blocklisted: by default headless Chromium's can't show canvases of a SwiftShader
// device (their textures are invalid). Chromium is driven with the DevTools protocol in a fresh
// profile, deleted afterwards.
import { existsSync, readdirSync } from 'node:fs';
import { homedir } from 'node:os';
import { join } from 'node:path';
import { type Browser, JsonSocket, jsonExpression, runChecker, startBrowser } from './pages.ts';

interface Options {
  hardware: boolean;
  chromium?: string;
}

// The newest headless shell in Playwright's browser caches
function playwrightShell(): string | null {
  const caches = [join(homedir(), 'Library/Caches/ms-playwright'), join(homedir(), '.cache/ms-playwright')];
  const found: { version: number; path: string }[] = [];
  for (const cache of caches) {
    if (!existsSync(cache)) continue;
    for (const entry of readdirSync(cache)) {
      const version = /^chromium_headless_shell-(\d+)$/.exec(entry);
      if (!version) continue;
      for (const platform of readdirSync(join(cache, entry))) {
        const path = join(cache, entry, platform, 'chrome-headless-shell');
        if (platform.startsWith('chrome-headless-shell-') && existsSync(path)) found.push({ version: Number(version[1]), path });
      }
    }
  }
  found.sort((a, b) => b.version - a.version);
  return found[0]?.path ?? null;
}

// The DevTools protocol's key events for a character: the code is what pages see (KeyboardEvent.code)
function keyEvent(key: string): { key: string; code: string; text: string; windowsVirtualKeyCode: number } {
  if (/^[a-z]$/i.test(key)) return { key, code: `Key${key.toUpperCase()}`, text: key, windowsVirtualKeyCode: key.toUpperCase().charCodeAt(0) };
  if (/^\d$/.test(key)) return { key, code: `Digit${key}`, text: key, windowsVirtualKeyCode: key.charCodeAt(0) };
  if (key === ' ') return { key, code: 'Space', text: key, windowsVirtualKeyCode: 32 };
  throw new Error(`can't press '${key}': only letters, digits and spaces`);
}

// A DevTools protocol command's result
async function send(cdp: JsonSocket, method: string, params: object = {}, sessionId?: string): Promise<any> {
  const reply = await cdp.request({ method, params, ...(sessionId ? { sessionId } : {}) });
  if (reply.error) throw new Error(`${method}: ${reply.error.message}`);
  return reply.result;
}

// A console message's text
function consoleText(args: { type: string; value?: unknown; description?: string }[]): string {
  return args.map((a) => (a.type === 'string' ? String(a.value) : a.description ?? JSON.stringify(a.value))).join(' ');
}

await runChecker<Options>('chromium', {
  options: {
    hardware: { type: 'boolean', default: false },
    chromium: { type: 'string', default: process.env.CHROMIUM },
  },
  usage: '[--hardware] [--chromium PATH]',
  async launch(options, log): Promise<Browser> {
    const binary = options.chromium ?? playwrightShell();
    if (!binary) {
      console.error('No Chromium: set CHROMIUM or --chromium, or run npx playwright install chromium-headless-shell');
      process.exit(3);
    }
    const graphics = options.hardware ? ['--enable-gpu'] : ['--use-angle=swiftshader'];
    const chromium = await startBrowser('Chromium', binary, (profile) => ['--headless', '--remote-debugging-port=0',
      `--user-data-dir=${profile}`, '--no-first-run', '--enable-unsafe-webgpu', ...graphics, '--window-size=1024,900',
      'about:blank'], /DevTools listening on (ws:\/\/\S+)/);
    let cdp: JsonSocket;
    let session: string;
    let loaded: () => void = () => {};
    try {
      cdp = await JsonSocket.connect(chromium.address, (message) => {
        if (message.method === 'Runtime.consoleAPICalled') log(`[${message.params.type}] ${consoleText(message.params.args)}`);
        else if (message.method === 'Runtime.exceptionThrown') log(`[exception] ${message.params.exceptionDetails.exception?.description ?? message.params.exceptionDetails.text}`);
        else if (message.method === 'Page.loadEventFired') loaded();
      });
      const { targetId } = await send(cdp, 'Target.createTarget', { url: 'about:blank' });
      session = (await send(cdp, 'Target.attachToTarget', { targetId, flatten: true })).sessionId;
      await send(cdp, 'Page.enable', {}, session);
      await send(cdp, 'Runtime.enable', {}, session);
    } catch (error) {
      await chromium.stop();
      throw error;
    }
    const evaluate = async (expression: string): Promise<unknown> => {
      const result = await send(cdp, 'Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true }, session);
      if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description ?? result.exceptionDetails.text);
      return result.result.value;
    };
    return {
      async navigate(url) {
        const load = new Promise<void>((done) => { loaded = done; });
        const { errorText } = await send(cdp, 'Page.navigate', { url }, session);
        if (errorText) throw new Error(`can't open ${url}: ${errorText}`);
        await load;
      },
      async evaluate(expression) {
        return JSON.parse((await evaluate(jsonExpression(expression))) as string);
      },
      async pressKey(key) {
        const event = keyEvent(key);
        await send(cdp, 'Input.dispatchKeyEvent', { type: 'keyDown', ...event }, session);
        await send(cdp, 'Input.dispatchKeyEvent', { type: 'keyUp', ...event }, session);
      },
      async screenshot(id) {
        const [x, y, width, height] = (await evaluate(`(() => {
          const r = document.getElementById(${JSON.stringify(id)}).getBoundingClientRect();
          return [r.x + scrollX, r.y + scrollY, r.width, r.height];
        })()`)) as number[];
        const shot = await send(cdp, 'Page.captureScreenshot',
          { format: 'png', clip: { x, y, width, height, scale: 1 }, captureBeyondViewport: true }, session);
        return Buffer.from(shot.data, 'base64');
      },
      async close() {
        cdp.close();
        await chromium.stop();
      },
    };
  },
});
