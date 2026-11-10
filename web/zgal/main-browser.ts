// Running a generated main in a page. mountProgram runs a program in a canvas of any page (each
// generated main's mount(canvas, options) calls it), so pages can embed programs, several at once.
// runHeadless runs a program headless (a program without a window, or one with, for --frames N), its
// path arguments' files given by the page and its captures returned as PNG Blobs; mountHeadless puts
// a form running it in an element (a windowless program's main's mount(container, options) calls
// it). runPage runs one as its own page (the generated FILE.zgal.html): its options from the query
// string (?blur&level=3&frames=60; arg=... for positional arguments, in order), drawing into the
// page's canvas (#zgal, else a new one), or for a program without a window showing its form, with
// the page's state in <html data-zgal="running|stopped|failed"> (stopped: the app quit or ran its
// --frames), so tools (web/tools/firefox.ts, chromium.ts) can wait for it.

import { Context, type AssetLoader, type AssetStreamLoader } from './context.ts';
import { RenderGraph } from './graph.ts';
import { Launch, parseCommandLine, runHeadlessFrames, type Program, type RunnableApp } from './main.ts';
import { encodePngStream } from './png-write.ts';
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

// Each button's bit in PointerEvent.buttons
const buttonBit: Record<MouseButton, number> = { left: 1, right: 2, middle: 4 };

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
  // Called as its assets' bytes arrive (see AssetProgress)
  onProgress?: AssetProgress;
}

export interface MountedProgram {
  // Stops the program, while it starts too, and frees what it used (its WebGPU device, its graphs,
  // the canvas's configuration and the listeners on the page), giving the canvas back its own
  // touch-action. The same promise each time.
  stop(): Promise<void>;
}

// Runs a program in a canvas: at its --size (or the size the app sets), shown as scaling says at the
// centre of the canvas's parent, which must have a size of its own (the canvas is positioned
// absolutely in it, and the parent made position: relative if it was static; the canvas has
// touch-action: none until stop() gives it back its own, so touches are the program's). Each mounted
// program has its own WebGPU device. assets: the URL its assets are fetched from (the generated
// main's mount gives assets/ beside it).
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
  // Touches on the canvas are the program's, not the page's to scroll or zoom, until it stops
  const touchAction = canvas.style.touchAction;
  canvas.style.touchAction = 'none';
  signal.addEventListener('abort', () => { canvas.style.touchAction = touchAction; });
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
    const parsed = parseCommandLine(program, [...(options.args ?? [])], inputs, 'canvas');
    if (!(parsed instanceof Launch)) {
      message(parsed.text);
      setState(parsed.status === 0 ? 'stopped' : 'failed');
      return;
    }
    const launch = parsed;
    resize(launch.size);
    try {
      context = await Context.create(assetLoaders(assets, {}, options.onProgress));
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
    // What is held, so each press gets one release: from its keyup or mouseup, or, as GLFW does
    // natively, when focus leaves (the keyup or mouseup then goes elsewhere, or comes later and is
    // ignored)
    const heldKeys = new Set<Key>();
    const heldButtons = new Set<MouseButton>();
    // The pointer whose buttons are held (see the pointer events below)
    let pressing: number | null = null;
    let lastCursor: [number, number] = [0, 0];
    const pressKey = (key: Key) => {
      if (heldKeys.has(key)) return;
      heldKeys.add(key);
      app.handle({ kind: 'key-pressed', key });
    };
    const releaseKey = (key: Key) => {
      if (heldKeys.delete(key)) app.handle({ kind: 'key-released', key });
    };
    const pressButton = (button: MouseButton, at: [number, number]) => {
      lastCursor = at;
      if (heldButtons.has(button)) return;
      heldButtons.add(button);
      app.handle({ kind: 'mouse-pressed', button, cursor: at });
    };
    const releaseButton = (button: MouseButton, at: [number, number]) => {
      lastCursor = at;
      if (heldButtons.delete(button)) app.handle({ kind: 'mouse-released', button, cursor: at });
    };
    const releaseAll = () => {
      for (const key of [...heldKeys]) releaseKey(key);
      for (const button of [...heldButtons]) releaseButton(button, lastCursor);
      pressing = null;
    };
    keys.addEventListener('keydown', (e) => {
      const event = e as KeyboardEvent;
      const key = keyOf(event.code);
      if (!key) return;
      event.preventDefault();
      // A repeat presses a key held while focus came back
      pressKey(key);
    }, { signal });
    keys.addEventListener('keyup', (e) => {
      const key = keyOf((e as KeyboardEvent).code);
      if (key) releaseKey(key);
    }, { signal });
    window.addEventListener('blur', releaseAll, { signal });
    if (keys !== window) keys.addEventListener('blur', releaseAll, { signal });
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'hidden') releaseAll();
    }, { signal });
    canvas.addEventListener('contextmenu', (event) => event.preventDefault(), { signal });
    // Pointer events, from a mouse, a pen or a finger, so a one-finger drag is a left-button drag.
    // Only a primary pointer's (the mouse, or the first finger down): further fingers are ignored.
    // A press follows its pointer until its buttons are all released (or it's cancelled), ignoring
    // others meanwhile (a mouse and a finger are both primary). Capturing the pointer keeps a drag's
    // moves and its release coming to the canvas when they leave it.
    const ours = (event: PointerEvent) => (pressing === null ? event.isPrimary : event.pointerId === pressing);
    canvas.addEventListener('pointerdown', (event) => {
      if (!ours(event)) return;
      pressing = event.pointerId;
      try {
        canvas.setPointerCapture(event.pointerId);
      } catch {
        // A pointer no longer active (e.g. lifted before this ran) has nothing to capture
      }
      if (options.keys !== 'page') canvas.focus();
      const button = buttonOf(event.button);
      if (button) pressButton(button, cursor(event));
    }, { signal });
    canvas.addEventListener('pointerup', (event) => {
      if (!ours(event)) return;
      if (event.buttons === 0) pressing = null;
      const button = buttonOf(event.button);
      if (button) releaseButton(button, cursor(event));
    }, { signal });
    canvas.addEventListener('pointermove', (event) => {
      if (!ours(event)) return;
      const at = cursor(event);
      // A button pressed or released while another is held comes as a move whose button is the one
      // that changed (a chord), possibly without moving
      const button = event.button >= 0 ? buttonOf(event.button) : null;
      if (!button || at[0] !== lastCursor[0] || at[1] !== lastCursor[1]) {
        lastCursor = at;
        app.handle({ kind: 'cursor-moved', cursor: at });
      }
      if (button) {
        if (event.buttons & buttonBit[button]) pressButton(button, at);
        else releaseButton(button, at);
      }
    }, { signal });
    // The browser taking the pointer (e.g. to scroll), or the canvas losing its capture (releasePointerCapture,
    // or another element capturing the pointer, so the release goes elsewhere), ends its drag.
    // Capture is lost after every release too, when nothing is pressing. (A canvas removed mid-drag
    // gets lostpointercapture on the document instead, and hiding it doesn't drop the capture.)
    const cancel = (event: PointerEvent) => {
      if (pressing === null || event.pointerId !== pressing) return;
      pressing = null;
      for (const button of [...heldButtons]) releaseButton(button, lastCursor);
    };
    canvas.addEventListener('pointercancel', cancel, { signal });
    canvas.addEventListener('lostpointercapture', cancel, { signal });
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

// Files a page gives a program, by the path its command line names them with: a File or Blob, or a
// URL (relative to the page) to fetch
export type ProgramFiles = Readonly<Record<string, Blob | URL | string>>;

// How much of an asset (read whole or streamed) has arrived, by its path as the program opens it:
// called once it's opened (loaded 0), then as each piece of it arrives, counted as it passes (it
// neither slows nor buffers the asset). total is its size when known: a Blob's size, or a response's
// Content-Length (unless it's compressed, or proves too small); else null until the asset has
// ended, when a last call gives total = loaded.
export type AssetProgress = (path: string, loaded: number, total: number | null) => void;

// A response's size from its Content-Length, unless its body is compressed (Content-Length is then
// the compressed size)
function contentLength(response: Response): number | null {
  const length = response.headers.get('Content-Length');
  const encoding = response.headers.get('Content-Encoding');
  if (length === null || (encoding !== null && encoding.trim().toLowerCase() !== 'identity')) return null;
  const size = Number(length);
  return length.trim() !== '' && Number.isSafeInteger(size) && size >= 0 ? size : null;
}

// A program's assets, read whole and streamed: the files given, else fetched from assets (relative
// paths, e.g. the command line's, resolve against it), their bytes counted for onProgress if given
function assetLoaders(assets: URL, files: ProgramFiles, onProgress: AssetProgress | undefined): { assets: AssetLoader; assetStreams: AssetStreamLoader } {
  const find = async (path: string): Promise<Blob | Response> => {
    const given = Object.hasOwn(files, path) ? files[path] : undefined;
    if (given instanceof Blob) return given;
    const response = await fetch(given === undefined ? new URL(path, assets) : new URL(given, globalThis.location?.href));
    if (!response.ok) throw new Error(`${response.status} ${response.statusText}`);
    return response;
  };
  const open = (path: string, found: Blob | Response): ReadableStream<Uint8Array> => {
    const source = (found instanceof Blob ? found.stream() : found.body) ?? new Blob([]).stream();
    if (!onProgress) return source;
    let total = found instanceof Blob ? found.size : contentLength(found);
    let loaded = 0;
    const counted = source.pipeThrough(new TransformStream<Uint8Array, Uint8Array>({
      transform(chunk, controller) {
        controller.enqueue(chunk);
        loaded += chunk.byteLength;
        // E.g. a cross-origin response whose compression is hidden
        if (total !== null && loaded > total) total = null;
        onProgress(path, loaded, total);
      },
      flush() {
        if (total === null) onProgress(path, loaded, loaded);
      },
    }));
    // Opened: if onProgress throws, the asset fails, and its source (e.g. a fetch's body) is closed
    try {
      onProgress(path, loaded, total);
    } catch (error) {
      void counted.cancel(error);
      throw error;
    }
    return counted;
  };
  return {
    assets: async (path) => {
      const found = await find(path);
      if (!onProgress) return new Uint8Array(await found.arrayBuffer());
      return new Uint8Array(await new Response(open(path, found)).arrayBuffer());
    },
    assetStreams: async (path) => open(path, await find(path)),
  };
}

// A capture a headless program made, as Node would write it to a PNG file
export interface Capture {
  // The file Node would write: the capture's path (--capture, or capture #:to's argument), or for
  // --capture-frames <path>/<frame>.png
  path: string;
  // --capture-frames' frame (from 1), or null for the last frame's capture
  frame: number | null;
  size: [number, number];
  // RGBA8 pixels, rows top to bottom
  rgba: Uint8Array;
  // The PNG file (image/png)
  png: Blob;
}

export interface HeadlessOptions {
  // The program's command line, as under Node (e.g. ['photo.jpg', 'out.png', '--blur', '2'])
  args: readonly string[];
  // The files the program reads by the paths args give them (see ProgramFiles); other paths are
  // fetched relative to its assets
  files?: ProgramFiles;
  // Called with each capture, once it's read
  onCapture?(capture: Capture): void;
  // Called as its assets' bytes arrive (see AssetProgress)
  onProgress?: AssetProgress;
  // Called as the program's state changes
  onState?(state: ProgramState): void;
  // The program's messages (errors, usage), by default to the console
  onMessage?(text: string): void;
}

export interface HeadlessProgram extends MountedProgram {
  // The state it ended in: stopped (it ran its frames, quit, printed its usage or was stopped) or
  // failed (a bad command line, an error, or WebGPU errors). The same promise each time.
  readonly done: Promise<'stopped' | 'failed'>;
}

// Runs a program headless, as under Node: its frames (--frames, or 1), its captures given to
// onCapture rather than written to files. It has its own WebGPU device, freed when it ends.
// assets: the URL its assets are fetched from (the generated main's runHeadless gives assets/ beside
// it).
export function runHeadless<A extends RunnableApp, I>(program: Program<A, I>, assets: URL, options: HeadlessOptions): HeadlessProgram {
  const message = options.onMessage ?? ((text: string) => console.log(text));
  // (cast so that TypeScript doesn't narrow it to null: the callbacks below set it)
  let state = null as ProgramState | null;
  const setState = (s: ProgramState) => {
    if (state === 'failed' || state === 'stopped') return;
    state = s;
    options.onState?.(s);
  };
  const fail = (error: unknown) => {
    message(`${program.name}: ${error instanceof Error ? error.message : String(error)}`);
    setState('failed');
  };
  let stopping = false;
  let context: Context | null = null;
  const graphs: RenderGraph[] = [];

  const run = async (): Promise<void> => {
    const inputs = program.defaults();
    const parsed = parseCommandLine(program, [...options.args], inputs, 'browser');
    if (!(parsed instanceof Launch)) {
      message(parsed.text);
      setState(parsed.status === 0 ? 'stopped' : 'failed');
      return;
    }
    const launch = parsed;
    context = await Context.create(assetLoaders(assets, options.files ?? {}, options.onProgress));
    if (stopping) return;
    const ready = context;
    const app = await program.create(ready, launch, inputs, () => {
      const graph = new RenderGraph(ready);
      graph.setSurfaceExtent(launch.size);
      graphs.push(graph);
      return graph;
    });
    if (stopping) return;
    setState('running');
    await runHeadlessFrames(program, app, launch, graphs, async ({ pixels, size }, frame) => {
      const path = frame === null ? launch.capture! : `${launch.capture}/${frame}.png`;
      const png = new Blob([await encodePngStream(size[0], size[1], pixels)], { type: 'image/png' });
      options.onCapture?.({ path, frame, size, rgba: pixels, png });
    }, () => stopping);
  };

  const done = (async (): Promise<'stopped' | 'failed'> => {
    try {
      await run();
    } catch (error) {
      fail(error);
    }
    const ended = context as Context | null;
    if (ended) {
      try {
        // The frames it submitted (all of them once runHeadlessFrames has finished)
        await ended.device.queue.onSubmittedWorkDone();
      } catch (error) {
        fail(error);
      }
      // WebGPU's validation errors fail the run, as they fail runs under Node
      const errors = ended.errors.splice(0);
      for (const error of errors) message(`[WebGPU] ${error}`);
      if (errors.length > 0) setState('failed');
      for (const graph of graphs) graph.destroy();
      ended.destroy();
    }
    setState('stopped');
    return state === 'failed' ? 'failed' : 'stopped';
  })();
  return {
    done,
    stop() {
      stopping = true;
      return done.then(() => {});
    },
  };
}

export interface HeadlessMountOptions {
  // A command line to run at once (a page's query string), as the form would give it
  args?: readonly string[];
  // Called as each run starts, with its command line
  onRun?(args: readonly string[]): void;
  // Called as each run's state changes
  onState?(state: ProgramState): void;
  // The runs' messages (errors, usage), by default to the console
  onMessage?(text: string): void;
}

// The last part of a path
function baseName(path: string): string {
  return path.slice(path.lastIndexOf('/') + 1);
}

// Puts a form running a program headless (runHeadless) in an element: an input per positional
// argument, option and flag, from the program's description (a file input for each path it reads;
// where its capture goes is named for it), the number of frames, and a Run button, followed by the
// last run's captures, each an image and a link downloading it. Running again stops the runs before
// (each has its own device), then clears their captures.
// The form is <form class="zgal-headless">, its fields <label class="zgal-field">, and the captures
// <figure>s in <div class="zgal-captures">, for the page's styles. stop() stops the run and removes
// what it added.
export function mountHeadless<A extends RunnableApp, I>(container: HTMLElement, program: Program<A, I>, assets: URL,
  options: HeadlessMountOptions = {}): MountedProgram {
  // An element with properties (e.g. an input's type) and children
  const make = <K extends keyof HTMLElementTagNameMap>(tag: K, properties: object = {}, ...children: (Node | string)[]): HTMLElementTagNameMap[K] => {
    const element: HTMLElementTagNameMap[K] = Object.assign(document.createElement(tag), properties);
    element.append(...children);
    return element;
  };
  const form = make('form', { className: 'zgal-headless' });
  const captures = make('div', { className: 'zgal-captures' });
  if (program.about) form.append(make('p', { className: 'zgal-about' }, program.about));

  // Each field adds its arguments, and its files by the paths the arguments give them
  type Field = (args: string[], files: Record<string, Blob>) => void;
  const fields: Field[] = [];
  // A field's label: [before] name [after] help
  const field = (name: string, help: string, before: HTMLElement | null, after: HTMLElement | null) => {
    const parts = [before, make('span', { className: 'zgal-name' }, name), after, make('small', {}, help)];
    form.append(make('label', { className: 'zgal-field' }, ...parts.filter((p): p is HTMLElement => p !== null)));
  };
  const numberInput = (option: (typeof program.options)[number]) => {
    const input = make('input', { type: 'number', required: true, step: option.value === 'float' ? 'any' : '1' });
    if (option.value === 'uint') input.min = '0';
    if (option.range) [input.min, input.max] = [String(option.range[0]), String(option.range[1])];
    if (option.default !== null) input.value = option.default;
    return input;
  };
  // A file input's file under its name (or n/name when another file has that name), giving the name
  const fileArgument = (input: HTMLInputElement, files: Record<string, Blob>): string => {
    const file = input.files![0];
    // A name like -x.png would be read as an option
    const given = file.name.startsWith('-') ? `./${file.name}` : file.name;
    let name = given;
    for (let n = 2; Object.hasOwn(files, name); n++) name = `${n}/${file.name}`;
    files[name] = file;
    return name;
  };
  // Where the capture goes, which the page names
  const output = `${program.name}.png`;
  for (const option of program.options) {
    const flag = option.kind === 'positional' ? [] : [option.name];
    if (option.output) {
      fields.push((args) => args.push(...flag, output));
    } else if (option.kind === 'flag') {
      const box = make('input', { type: 'checkbox' });
      field(option.name, option.help, box, null);
      fields.push((args) => { if (box.checked) args.push(option.name); });
    } else if (option.value === 'path') {
      // Positionals are needed; an option is given when it has a file
      const input = make('input', { type: 'file', required: option.kind === 'positional' });
      field(option.kind === 'positional' ? option.metavar : option.name, option.help, null, input);
      fields.push((args, files) => { if (input.files?.length) args.push(...flag, fileArgument(input, files)); });
    } else if (option.kind === 'positional') {
      const input = numberInput(option);
      field(option.metavar, option.help, null, input);
      fields.push((args) => args.push(input.value));
    } else {
      // An option is given when checked, which changing its value does
      const box = make('input', { type: 'checkbox' });
      const input = numberInput(option);
      input.addEventListener('input', () => { box.checked = true; });
      field(option.name, option.help, box, input);
      fields.push((args) => { if (box.checked) args.push(option.name, input.value); });
    }
  }
  const frames = make('input', { type: 'number', required: true, min: '1', step: '1', value: '1' });
  field('--frames', 'the frames to run', null, frames);
  fields.push((args) => args.push('--frames', frames.value));
  if (program.captureOption) fields.push((args) => args.push('--capture', output));
  form.append(make('button', { type: 'submit' }, 'Run'));
  container.append(form, captures);

  // Every run not yet ended, for start and stop() to wait for
  const live = new Set<HeadlessProgram>();
  let runs = 0;
  let unmounted = false;
  let urls: string[] = [];
  const clear = () => {
    for (const url of urls) URL.revokeObjectURL(url);
    urls = [];
    captures.replaceChildren();
  };
  // Stops the runs before, then runs (only the last of several started meanwhile runs). A run's
  // callbacks stop once another has been started, so a stopped run's states, messages and captures
  // never follow the next run's onRun (its messages go to console.debug); unmounting still reports
  // the run's last messages and its final 'stopped'.
  const start = async (args: readonly string[], files: ProgramFiles) => {
    const run = ++runs;
    await Promise.all([...live].map((before) => before.stop()));
    if (run !== runs || unmounted) return;
    clear();
    options.onRun?.(args);
    const started = runHeadless(program, assets, {
      args,
      files,
      onState: (state) => { if (run === runs || unmounted) options.onState?.(state); },
      onMessage: (text) => { (run === runs || unmounted ? options.onMessage ?? console.log : console.debug)(text); },
      onCapture: (capture) => {
        if (run !== runs) return;
        const url = URL.createObjectURL(capture.png);
        urls.push(url);
        const name = baseName(capture.path);
        captures.append(make('figure', {},
          make('img', { src: url, alt: name, width: capture.size[0], height: capture.size[1] }),
          make('figcaption', {}, make('a', { href: url, download: name }, `Download ${name}`))));
      },
    });
    live.add(started);
    void started.done.then(() => live.delete(started));
  };
  form.addEventListener('submit', (event) => {
    event.preventDefault();
    const args: string[] = [];
    const files: Record<string, Blob> = {};
    for (const f of fields) f(args, files);
    void start(args, files);
  });
  if (options.args) void start(options.args, {});

  let stopped: Promise<void> | null = null;
  return {
    stop() {
      stopped ??= (async () => {
        unmounted = true;
        runs++;
        await Promise.all([...live].map((run) => run.stop()));
        clear();
        form.remove();
        captures.remove();
      })();
      return stopped;
    },
  };
}

// Clears the page's messages
function clearMessages(): void {
  const element = document.getElementById('zgal-message');
  if (element) element.textContent = '';
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
  if (!program.window) {
    runHeadlessPage(program, url, args);
    return;
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

// Runs a program without a window as the page: its form (mountHeadless) in #zgal-headless (else a
// new element), running args at once when the query string gives a command line. A path the query
// string gives is fetched relative to the program's assets (a URL from the root, /..., from there).
function runHeadlessPage<A extends RunnableApp, I>(program: Program<A, I>, url: string, args: string[]): void {
  document.title = program.name;
  const container = document.getElementById('zgal-headless') ?? document.body.appendChild(Object.assign(document.createElement('main'), { id: 'zgal-headless' }));
  mountHeadless(container, program, new URL('assets/', url), {
    ...(args.length > 0 ? { args } : {}),
    onRun: () => {
      clearMessages();
      delete document.documentElement.dataset.zgal;
    },
    onState: (state) => { document.documentElement.dataset.zgal = state; },
    onMessage: show,
  });
}
