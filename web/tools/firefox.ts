// Runs a ZGAL page (FILE.zgal.html) in Firefox and checks it: prints the page's console and messages,
// optionally presses keys, screenshots its canvas and compares it with an approved PNG. Exits with 0
// when the page ran without errors (and matched), 1 when it failed, didn't match or timed out, 2 for a
// bad command line, 3 when there's no Firefox.
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
// Firefox is driven with WebDriver BiDi in a fresh profile, deleted afterwards. chromium.ts checks
// pages the same way in headless Chromium; pages.ts has what they share.
import { existsSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { type Browser, JsonSocket, jsonExpression, runChecker, startBrowser } from './pages.ts';

interface Options {
  headless: boolean;
  firefox: string;
}

// A WebDriver BiDi command's result
async function send(bidi: JsonSocket, method: string, params: object): Promise<any> {
  const reply = await bidi.request({ method, params });
  if (reply.type === 'error') throw new Error(`${method}: ${reply.error}: ${reply.message}`);
  return reply.result;
}

await runChecker<Options>('firefox', {
  options: {
    headless: { type: 'boolean', default: false },
    firefox: { type: 'string', default: process.env.FIREFOX ?? '/Applications/Firefox.app/Contents/MacOS/firefox' },
  },
  usage: '[--headless] [--firefox PATH]',
  async launch(options, log): Promise<Browser> {
    if (!existsSync(options.firefox)) {
      console.error(`No Firefox at ${options.firefox}: set FIREFOX or --firefox`);
      process.exit(3);
    }
    // Port 0: Firefox picks a free one and prints it. A fresh profile and instance, so a Firefox
    // already open isn't touched; the window is large enough to show a 256x240 canvas at 3x.
    const firefox = await startBrowser('Firefox', options.firefox, (profile) => {
      writeFileSync(join(profile, 'user.js'), [
        'user_pref("dom.webgpu.enabled", true);',
        'user_pref("browser.shell.checkDefaultBrowser", false);',
        'user_pref("datareporting.policy.dataSubmissionEnabled", false);',
        'user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);',
      ].join('\n'));
      return ['--remote-debugging-port', '0', '--profile', profile, '--no-remote', '--new-instance',
        '--window-size', '1024,900', ...(options.headless ? ['--headless'] : [])];
    }, /WebDriver BiDi listening on (ws:\/\/\S+)/);
    let bidi: JsonSocket;
    let context: string;
    try {
      bidi = await JsonSocket.connect(`${firefox.address}/session`, (message) => {
        if (message.method === 'log.entryAdded') log(`[${message.params.level}] ${message.params.text}`);
      });
      await send(bidi, 'session.new', { capabilities: {} });
      await send(bidi, 'session.subscribe', { events: ['log.entryAdded'] });
      context = (await send(bidi, 'browsingContext.getTree', {})).contexts[0].context;
    } catch (error) {
      await firefox.stop();
      throw error;
    }
    const evaluate = async (expression: string) =>
      (await send(bidi, 'script.evaluate', { target: { context }, expression, awaitPromise: true })).result;
    return {
      async navigate(url) {
        await send(bidi, 'browsingContext.navigate', { context, url, wait: 'complete' });
      },
      async evaluate(expression) {
        return JSON.parse((await evaluate(jsonExpression(expression))).value);
      },
      async pressKey(key) {
        await send(bidi, 'input.performActions', { context, actions: [{ type: 'key', id: 'keyboard',
          actions: [{ type: 'keyDown', value: key }, { type: 'pause', duration: 50 }, { type: 'keyUp', value: key }] }] });
      },
      async screenshot(id) {
        const element = await evaluate(`document.getElementById(${JSON.stringify(id)})`);
        const shot = await send(bidi, 'browsingContext.captureScreenshot',
          { context, clip: { type: 'element', element: { sharedId: element.sharedId } } });
        return Buffer.from(shot.data, 'base64');
      },
      async close() {
        bidi.close();
        await firefox.stop();
      },
    };
  },
});
