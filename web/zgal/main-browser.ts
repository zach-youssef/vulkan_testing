// Running a generated main in a page: its options from the query string (?blur&level=3&frames=60;
// arg=... for positional arguments, in order), drawing into the page's canvas (#zgal, else a new one)
// each animation frame, with the canvas's keyboard, mouse and wheel events going to the app's
// handlers. The page's state is in <html data-zgal="running|stopped|failed"> (stopped: the app quit
// or ran its --frames), so tools (web/tools/firefox.ts) can wait for it.

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

function setState(state: 'running' | 'stopped' | 'failed'): void {
  document.documentElement.dataset.zgal = state;
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

export async function runPage<A extends RunnableApp, I>(program: Program<A, I>, url: string): Promise<void> {
  const args: string[] = [];
  for (const [key, value] of new URLSearchParams(location.search)) {
    if (key === 'arg') args.push(value);
    else args.push(`--${key}`, ...(value === '' ? [] : [value]));
  }
  const inputs = program.defaults();
  const parsed = parseCommandLine(program, args, inputs, 'page');
  if (!(parsed instanceof Launch)) {
    show(parsed.text);
    setState(parsed.status === 0 ? 'stopped' : 'failed');
    return;
  }
  const launch = parsed;
  document.title = program.window?.title ?? program.name;
  let canvas = document.getElementById('zgal') as HTMLCanvasElement | null;
  if (!canvas) {
    canvas = document.createElement('canvas');
    canvas.id = 'zgal';
    document.body.append(canvas);
  }
  const target = canvas;
  // The canvas holds the surface's pixels, shown at the largest whole multiple that fits the page
  const resize = (size: [number, number]) => {
    target.width = size[0];
    target.height = size[1];
    fit();
  };
  const fit = () => {
    const scale = Math.max(1, Math.floor(Math.min(innerWidth / target.width, innerHeight / target.height)));
    target.style.width = `${target.width * scale}px`;
    target.style.height = `${target.height * scale}px`;
  };
  addEventListener('resize', fit);
  resize(launch.size);

  const assetBase = new URL('assets/', url);
  let context: Context;
  try {
    context = await Context.create({
      assets: async (path) => {
        const response = await fetch(new URL(path, assetBase));
        if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
        return new Uint8Array(await response.arrayBuffer());
      },
    });
  } catch (error) {
    // E.g. no WebGPU adapter (headless browsers)
    show(`${program.name}: ${error instanceof Error ? error.message : String(error)}`);
    setState('failed');
    return;
  }
  const surface = target.getContext('webgpu');
  if (!surface) {
    show('This browser has no WebGPU canvas');
    setState('failed');
    return;
  }
  let app: A;
  try {
    app = await program.create(context, launch, inputs, () => {
      const graph = new RenderGraph(context);
      graph.setSurface(surface);
      return graph;
    });
  } catch (error) {
    show(`${program.name}: ${error instanceof Error ? error.message : String(error)}`);
    setState('failed');
    return;
  }
  setState('running');
  for (const line of program.controls) console.log(line);

  const cursor = (event: MouseEvent): [number, number] => {
    const rect = target.getBoundingClientRect();
    return [Math.fround(((event.clientX - rect.left) / rect.width) * 2 - 1), Math.fround(((event.clientY - rect.top) / rect.height) * 2 - 1)];
  };
  addEventListener('keydown', (event) => {
    const key = keyOf(event.code);
    if (!key) return;
    event.preventDefault();
    if (!event.repeat) app.handle({ kind: 'key-pressed', key });
  });
  addEventListener('keyup', (event) => {
    const key = keyOf(event.code);
    if (key) app.handle({ kind: 'key-released', key });
  });
  target.addEventListener('contextmenu', (event) => event.preventDefault());
  target.addEventListener('mousedown', (event) => {
    const button = buttonOf(event.button);
    if (button) app.handle({ kind: 'mouse-pressed', button, cursor: cursor(event) });
  });
  addEventListener('mouseup', (event) => {
    const button = buttonOf(event.button);
    if (button) app.handle({ kind: 'mouse-released', button, cursor: cursor(event) });
  });
  addEventListener('mousemove', (event) => app.handle({ kind: 'cursor-moved', cursor: cursor(event) }));
  target.addEventListener('wheel', (event) => {
    event.preventDefault();
    // In lines, y up, as GLFW gives them
    const lines = event.deltaMode === WheelEvent.DOM_DELTA_LINE ? 1 : event.deltaMode === WheelEvent.DOM_DELTA_PAGE ? 20 : 1 / 100;
    app.handle({ kind: 'scrolled', offset: [Math.fround(-event.deltaX * lines), Math.fround(-event.deltaY * lines)] });
  }, { passive: false });

  // Until the app quits, or for --frames. WebGPU errors don't stop it, but it ends failed.
  let frame = 0;
  let gpuErrors = false;
  const step = async () => {
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
      show(`${program.name}: ${error instanceof Error ? error.message : String(error)}`);
      setState('failed');
      return;
    }
    if (context.errors.length > 0) {
      for (const error of context.errors.splice(0)) show(`[WebGPU] ${error}`);
      gpuErrors = true;
    }
    requestAnimationFrame(step);
  };
  requestAnimationFrame(step);
}
