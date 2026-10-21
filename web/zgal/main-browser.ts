// Running a generated main in a page. mountProgram runs a program in a canvas of any page (each
// generated main's mount(canvas, options) calls it), so pages can embed programs, several at once.
// runPage runs one as its own page (the generated FILE.zgal.html): its options from the query string
// (?blur&level=3&frames=60; arg=... for positional arguments, in order), drawing into the page's
// canvas (#zgal, else a new one), with the page's state in <html data-zgal="running|stopped|failed">
// (stopped: the app quit or ran its --frames), so tools (web/tools/firefox.ts, chromium.ts) can wait
// for it.

import { Context } from './context.ts';
import { RenderGraph } from './graph.ts';
import { Launch, parseCommandLine, type Program, type RunnableApp } from './main.ts';
import type { Key, MouseButton } from './app.ts';

// KeyboardEvent.code to the keys apps handle
function keyOf(code: string): Key | null {
  if (/^Key[A-Z]$/.test(code)) return code.slice(3) as Key;
  if (/^(Digit\d|F\d\d?|Space|Escape|Enter|Tab|Backspace|Minus|Equal)$/.test(code)) return code as Key;
  const named: Record<string, Key> = {
    ArrowLeft: 'Left', ArrowRight: 'Right', ArrowUp: 'Up', ArrowDown: 'Down', NumpadAdd: 'KeypadAdd',
    NumpadSubtract: 'KeypadSubtract',
  };
  return named[code] ?? null;
}

function buttonOf(button: number): MouseButton | null {
  return button === 0 ? 'left' : button === 2 ? 'right' : button === 1 ? 'middle' : null;
}

// running: it started; stopped: the app quit, ran its --frames, printed its usage (--help) or was
// stopped; failed: it couldn't start, threw, or WebGPU reported errors
export type ProgramState = 'running' | 'stopped' | 'failed';

export interface MountOptions {
  // How the canvas is shown in its parent: at the largest whole multiple of its size that fits,
  // pixelated (for pixel art, and pixel-exact screenshots), or at the largest size that fits,
  // smoothly scaled
  scaling: 'whole-multiples' | 'fit';
  // The program's command line, as under Node (e.g. ['--frames', '60'])
  args?: readonly string[];
  // Where key presses come from: the canvas while it has focus (it takes focus when clicked; the
  // default), or the whole page (for a page that is only the program)
  keys?: 'canvas' | 'page';
  // Called as the program's state changes
  onState?(state: ProgramState): void;
  // The program's messages (errors, usage), by default to the console
  onMessage?(text: string): void;
}

export interface MountedProgram {
  // Stops the program, while it starts too, and frees what it used (its WebGPU device, its graphs,
  // the canvas's configuration and the listeners on the page). The same promise each time.
  stop(): Promise<void>;
}

// Runs a program in a canvas: at its --size (or the size the app sets), shown as scaling says at the
// centre of the canvas's parent, which must have a size of its own (the canvas is positioned
// absolutely in it, and the parent made position: relative if it was static). Each mounted program
// has its own WebGPU device. assets: the URL its assets are fetched from (the generated main's mount
// gives assets/ beside it).
export function mountProgram<A extends RunnableApp, I>(canvas: HTMLCanvasElement, program: Program<A, I>, assets: URL,
  options: MountOptions): MountedProgram {
  const message = options.onMessage ?? ((text: string) => console.log(text));
  let state: ProgramState | null = null;
  const setState = (s: ProgramState) => {
    state = s;
    options.onState?.(s);
  };
  const fail = (error: unknown) => {
    message(`${program.name}: ${error instanceof Error ? error.message : String(error)}`);
    setState('failed');
  };

  let stopping = false;
  const listeners = new AbortController();
  const signal = listeners.signal;
  let context: Context | null = null;
  let surface: GPUCanvasContext | null = null;
  const graphs: RenderGraph[] = [];
  let animation: number | null = null;
  let frameDone: Promise<void> = Promise.resolve();

  // The canvas holds the surface's pixels, shown scaled at the centre of its parent's content box
  const parent = canvas.parentElement;
  if (!parent) throw new Error('mountProgram: the canvas must be in a page, in a parent that sizes it');
  if (getComputedStyle(parent).position === 'static') parent.style.position = 'relative';
  canvas.style.position = 'absolute';
  canvas.style.imageRendering = options.scaling === 'whole-multiples' ? 'pixelated' : 'auto';
  let box = { left: 0, top: 0, width: parent.clientWidth, height: parent.clientHeight };
  const fit = () => {
    const ratio = Math.min(box.width / canvas.width, box.height / canvas.height);
    const scale = options.scaling === 'whole-multiples' ? Math.max(1, Math.floor(ratio)) : ratio;
    const width = Math.round(canvas.width * scale);
    const height = Math.round(canvas.height * scale);
    canvas.style.width = `${width}px`;
    canvas.style.height = `${height}px`;
    // Whole pixels, so a pixelated canvas's blocks stay whole
    canvas.style.left = `${box.left + Math.floor((box.width - width) / 2)}px`;
    canvas.style.top = `${box.top + Math.floor((box.height - height) / 2)}px`;
  };
  const resize = (size: readonly [number, number]) => {
    canvas.width = size[0];
    canvas.height = size[1];
    fit();
  };
  const observer = new ResizeObserver(([entry]) => {
    const r = entry.contentRect;
    box = { left: r.left, top: r.top, width: r.width, height: r.height };
    fit();
  });
  observer.observe(parent);
  signal.addEventListener('abort', () => observer.disconnect());

  const start = async (): Promise<void> => {
    const inputs = program.defaults();
    const parsed = parseCommandLine(program, [...(options.args ?? [])], inputs, 'page');
    if (!(parsed instanceof Launch)) {
      message(parsed.text);
      setState(parsed.status === 0 ? 'stopped' : 'failed');
      return;
    }
    const launch = parsed;
    resize(launch.size);
    try {
      context = await Context.create({
        assets: async (path) => {
          const response = await fetch(new URL(path, assets));
          if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
          return new Uint8Array(await response.arrayBuffer());
        },
      });
    } catch (error) {
      // E.g. no WebGPU adapter (headless browsers)
      fail(error);
      return;
    }
    if (stopping) return;
    surface = canvas.getContext('webgpu');
    if (!surface) {
      message('This browser has no WebGPU canvas');
      setState('failed');
      return;
    }
    const ready = context;
    const shown = surface;
    let app: A;
    try {
      app = await program.create(ready, launch, inputs, () => {
        const graph = new RenderGraph(ready);
        graph.setSurface(shown);
        graphs.push(graph);
        return graph;
      });
    } catch (error) {
      fail(error);
      return;
    }
    if (stopping) return;
    setState('running');

    const cursor = (event: MouseEvent): [number, number] => {
      const rect = canvas.getBoundingClientRect();
      return [Math.fround(((event.clientX - rect.left) / rect.width) * 2 - 1), Math.fround(((event.clientY - rect.top) / rect.height) * 2 - 1)];
    };
    const keys: EventTarget = options.keys === 'page' ? window : canvas;
    if (options.keys !== 'page' && !canvas.hasAttribute('tabindex')) canvas.tabIndex = 0;
    keys.addEventListener('keydown', (e) => {
      const event = e as KeyboardEvent;
      const key = keyOf(event.code);
      if (!key) return;
      event.preventDefault();
      if (!event.repeat) app.handle({ kind: 'key-pressed', key });
    }, { signal });
    keys.addEventListener('keyup', (e) => {
      const key = keyOf((e as KeyboardEvent).code);
      if (key) app.handle({ kind: 'key-released', key });
    }, { signal });
    canvas.addEventListener('contextmenu', (event) => event.preventDefault(), { signal });
    // Capturing the pointer keeps a drag's moves and its release coming to the canvas when they
    // leave it (the mouse events follow the capture)
    canvas.addEventListener('pointerdown', (event) => {
      canvas.setPointerCapture(event.pointerId);
      if (options.keys !== 'page') canvas.focus();
    }, { signal });
    canvas.addEventListener('mousedown', (event) => {
      const button = buttonOf(event.button);
      if (button) app.handle({ kind: 'mouse-pressed', button, cursor: cursor(event) });
    }, { signal });
    canvas.addEventListener('mouseup', (event) => {
      const button = buttonOf(event.button);
      if (button) app.handle({ kind: 'mouse-released', button, cursor: cursor(event) });
    }, { signal });
    canvas.addEventListener('mousemove', (event) => app.handle({ kind: 'cursor-moved', cursor: cursor(event) }), { signal });
    canvas.addEventListener('wheel', (event) => {
      event.preventDefault();
      // In lines, y up, as GLFW gives them
      const lines = event.deltaMode === WheelEvent.DOM_DELTA_LINE ? 1 : event.deltaMode === WheelEvent.DOM_DELTA_PAGE ? 20 : 1 / 100;
      app.handle({ kind: 'scrolled', offset: [Math.fround(-event.deltaX * lines), Math.fround(-event.deltaY * lines)] });
    }, { passive: false, signal });

    // Until the app quits, or for --frames. WebGPU errors don't stop it, but it ends failed.
    let frame = 0;
    let gpuErrors = false;
    const step = async () => {
      animation = null;
      if (stopping) return;
      if (app.quitRequested() || (launch.frames !== null && frame >= launch.frames)) {
        setState(gpuErrors ? 'failed' : 'stopped');
        return;
      }
      frame++;
      try {
        app.beginFrame();
        const size = app.takeResize();
        if (size) resize(size);
        await app.renderGraph().execute();
      } catch (error) {
        fail(error);
        return;
      }
      if (ready.errors.length > 0) {
        for (const error of ready.errors.splice(0)) message(`[WebGPU] ${error}`);
        gpuErrors = true;
      }
      if (!stopping) animation = requestAnimationFrame(() => { frameDone = step(); });
    };
    animation = requestAnimationFrame(() => { frameDone = step(); });
  };
  const started = start().catch(fail);

  let stopped: Promise<void> | null = null;
  return {
    stop() {
      stopped ??= (async () => {
        stopping = true;
        if (animation !== null) cancelAnimationFrame(animation);
        listeners.abort();
        await started;
        await frameDone;
        const ended = context;
        if (ended) {
          try {
            for (const graph of graphs) await graph.finish();
          } finally {
            for (const graph of graphs) graph.destroy();
            surface?.unconfigure();
            ended.destroy();
          }
        }
        if (state === null || state === 'running') setState('stopped');
      })();
      return stopped;
    },
  };
}

// Shows a message over the page (errors and usage), and in the console
function show(text: string): void {
  console.log(text);
  let element = document.getElementById('zgal-message');
  if (!element) {
    element = document.createElement('pre');
    element.id = 'zgal-message';
    document.body.append(element);
  }
  element.textContent = `${element.textContent ?? ''}${text}\n`;
}

// Runs a program as the page (url: the generated main's, which assets are found beside), in the
// page's canvas, which fills the page's body
export async function runPage<A extends RunnableApp, I>(program: Program<A, I>, url: string): Promise<void> {
  const args: string[] = [];
  for (const [key, value] of new URLSearchParams(location.search)) {
    if (key === 'arg') args.push(value);
    else args.push(`--${key}`, ...(value === '' ? [] : [value]));
  }
  document.title = program.window?.title ?? program.name;
  const canvas = (document.getElementById('zgal') as HTMLCanvasElement | null) ?? document.body.appendChild(Object.assign(document.createElement('canvas'), { id: 'zgal' }));
  await new Promise<void>((done) => {
    mountProgram(canvas, program, new URL('assets/', url), {
      scaling: 'whole-multiples',
      args,
      keys: 'page',
      onState: (state) => {
        document.documentElement.dataset.zgal = state;
        if (state === 'running') for (const line of program.controls) console.log(line);
        else done();
      },
      onMessage: show,
    });
  });
}
