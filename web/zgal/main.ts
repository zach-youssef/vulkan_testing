// What the mains ZGAL generates for the web (a file's `main`) use, wherever they run: the program's
// description, its command line (the built-in options and the app's, as include/zgal/Main.h parses
// them), and running it, under Node (main-node.ts: headless, captures to PNG files) or in a page
// (main-browser.ts: a canvas, events, animation frames).

import type { Context } from './context.ts';
import type { RenderGraph } from './graph.ts';
import { ReadbackNode } from './graph.ts';
import type { NodeId } from './graph.ts';
import { RunMode, Timestep, type Event } from './app.ts';

// What every generated app class has, for running it
export interface RunnableApp {
  beginFrame(): void;
  renderGraph(): RenderGraph;
  handle(event: Event): void;
  quitRequested(): boolean;
  takeResize(): [number, number] | null;
  setSurfaceExtent(extent: [number, number]): void;
}

// What the built-in options chose
export class Launch {
  frames: number | null = null; // --frames N, else until the page closes
  capture: string | null = null; // Where the last frame's capture goes
  captureFrames: number[] = []; // --capture-frames: the frames (from 1) captured instead, to <capture>/<frame>.png
  size: [number, number] = [0, 0]; // --size, or the window's
  window = false; // Whether it runs in a window (a page's canvas)

  // How the app is run (its headless, capture and frameLimit)
  mode(): RunMode {
    return this.window ? RunMode.window(this.frames ?? 0) : RunMode.headless(this.frames ?? 0, this.capture !== null);
  }

  // The app's clock: real time interactively, a fixed step for a number of frames, so those runs are
  // deterministic
  timestep(maxStep: number, fixedStep: number): Timestep {
    return this.frames !== null ? Timestep.fixed(fixedStep) : Timestep.realTime(maxStep);
  }
}

// An option's setter: parses its text into the app's inputs, giving an error message or null
export type Setter<I> = (inputs: I, text: string) => string | null;

export interface ProgramOption<I> {
  kind: 'flag' | 'option' | 'positional';
  name: string; // e.g. --blur, or <image> for a positional
  metavar: string;
  help: string;
  set: Setter<I>;
}

// A generated main's program: I is the app's parameters, which the command line sets
export interface Program<A extends RunnableApp, I> {
  name: string;
  about: string;
  window: { title: string; width: number; height: number } | null;
  captureOption: boolean;
  // Where a program capturing to a parameter (capture #:to) writes it, from the command line
  captureTo: ((inputs: I) => string) | null;
  controls: string[];
  options: ProgramOption<I>[];
  // The parameters before the command line: the options' defaults
  defaults(): I;
  // The app, its graphs from newGraph (which a page gives its canvas)
  create(context: Context, launch: Launch, parameters: I, newGraph: () => RenderGraph): Promise<A>;
  // The capture's readback (an RGBA8 image's), or null for a program that captures nothing
  capture: ((app: A) => { graph: RenderGraph; node: NodeId }) | null;
}

// Parses a number for an option, within [low, high] when given: an error message, or null
export function parseNumber(kind: 'int' | 'uint' | 'float', text: string, set: (value: number) => void,
  low: number | null = null, high: number | null = null): string | null {
  const pattern = kind === 'float' ? /^[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?$/ : kind === 'int' ? /^-?\d+$/ : /^\d+$/;
  const value = kind === 'float' ? Math.fround(Number(text)) : Number(text);
  const fits = kind === 'float' ? Number.isFinite(value) : kind === 'int' ? value >= -(2 ** 31) && value < 2 ** 31 : value < 2 ** 32;
  if (!pattern.test(text) || !fits) {
    return `expects ${kind === 'float' ? 'a number' : kind === 'int' ? 'a whole number' : 'a whole number of at least 0'}, not '${text}'`;
  }
  if ((low !== null && value < low) || (high !== null && value > high)) {
    return `must be from ${low ?? value} to ${high ?? value}`;
  }
  set(value);
  return null;
}

// A path given on the command line: under Node, relative to the working directory (asset
// directories don't apply to it); in a page, relative to the page
export function commandLinePath(text: string): string {
  const process = (globalThis as { process?: { cwd(): string; versions?: { node?: string } } }).process;
  if (!process?.versions?.node || text.startsWith('/')) return text;
  return `${process.cwd()}/${text}`;
}

// A node with a #:when, which must hold for the program to use it
export function requireNode(id: NodeId | undefined, graph: string, node: string): NodeId {
  if (id === undefined) throw new Error(`${graph}'s node ${node} isn't in the graph (its #:when is false)`);
  return id;
}

// The command line's built-in choices, or (for a bad command line, or --help) the exit status and
// what to print. Under Node the program never has a window; in a page it always has.
export function parseCommandLine<A extends RunnableApp, I>(program: Program<A, I>, args: string[], inputs: I,
  where: 'node' | 'page'): Launch | { status: number; text: string } {
  const launch = new Launch();
  launch.window = program.window !== null;
  if (program.window) launch.size = [program.window.width, program.window.height];
  const positionals = program.options.filter((o) => o.kind === 'positional');
  let positional = 0;
  const fail = (message: string) => ({ status: 2, text: `${program.name}: ${message}\n${usage(program)}` });
  for (let i = 0; i < args.length; i++) {
    const arg = args[i];
    const value = (): string | null => (i + 1 < args.length ? args[++i] : null);
    if (arg === '-h' || arg === '--help') {
      return { status: 0, text: usage(program) };
    } else if (arg === '--validation') {
      // WebGPU always validates
    } else if (arg === '--frames') {
      const v = value();
      if (v === null) return fail('--frames needs a value');
      const error = parseNumber('uint', v, (n) => (launch.frames = n), 1);
      if (error) return fail(`--frames ${error}`);
    } else if (arg === '--capture' && program.captureOption) {
      const v = value();
      if (v === null) return fail('--capture needs a value');
      launch.capture = v;
    } else if (arg === '--capture-frames' && program.captureOption) {
      const v = value();
      if (v === null) return fail('--capture-frames needs a value');
      for (const frame of v.split(',')) {
        const error = parseNumber('uint', frame, (n) => launch.captureFrames.push(n), 1);
        if (error) return fail(`--capture-frames takes frame numbers separated by commas, e.g. 1,2,60: ${error}`);
      }
      launch.captureFrames = [...new Set(launch.captureFrames)].sort((a, b) => a - b);
    } else if (arg === '--size' && program.window) {
      const match = /^(\d+)x(\d+)$/.exec(value() ?? '');
      if (!match || Number(match[1]) < 1 || Number(match[2]) < 1) return fail('--size takes WIDTHxHEIGHT, e.g. 640x360');
      launch.size = [Number(match[1]), Number(match[2])];
    } else if (arg.startsWith('--')) {
      const option = program.options.find((o) => o.kind !== 'positional' && o.name === arg);
      if (!option) return fail(`unknown option ${arg}`);
      let text = '';
      if (option.kind === 'option') {
        const v = value();
        if (v === null) return fail(`${arg} needs a value`);
        text = v;
      }
      const error = option.set(inputs, text);
      if (error) return fail(`${arg} ${error}`);
    } else {
      if (positional >= positionals.length) return fail(`unexpected argument ${arg}`);
      const error = positionals[positional].set(inputs, arg);
      if (error) return fail(`${positionals[positional].metavar} ${error}`);
      positional++;
    }
  }
  if (positional < positionals.length) return fail(`expected ${positionals[positional].metavar}`);
  if (program.captureTo && where === 'node') launch.capture = program.captureTo(inputs);
  // Capturing several frames runs as many frames as the last one needs
  if (launch.captureFrames.length > 0) {
    const last = launch.captureFrames[launch.captureFrames.length - 1];
    if (launch.capture === null) return fail('--capture-frames writes into the directory --capture DIRECTORY names');
    if (launch.frames === null) launch.frames = last;
    else if (launch.frames < last) return fail(`--capture-frames needs ${last} frames, but --frames is ${launch.frames}`);
  }
  if (where === 'page') {
    // A page draws into its canvas, for --frames N frames or until it closes
    if (!program.window) return fail('runs headless only: run it under Node');
    if (launch.capture !== null) return fail('--capture runs headless: run it under Node');
    launch.window = true;
  } else {
    if (program.window && launch.frames === null) return fail('runs headless under Node: give --frames N (or open its page)');
    launch.window = false;
    launch.frames ??= 1;
  }
  return launch;
}

export function usage<A extends RunnableApp, I>(program: Program<A, I>): string {
  const positionals = program.options.filter((o) => o.kind === 'positional');
  const lines: [string, string][] = [
    ...positionals.map((p): [string, string] => [p.metavar, p.help]),
    ...program.options.filter((o) => o.kind !== 'positional').map((o): [string, string] => [o.name + (o.metavar ? ` ${o.metavar}` : ''), o.help]),
  ];
  if (program.window) {
    lines.push(['--size WxH', `the canvas's size, or headless the image's (default ${program.window.width}x${program.window.height})`]);
  }
  lines.push(['--frames N', program.window ? 'run N frames, then exit (under Node, headless)' : 'run N frames (default 1)']);
  if (program.captureOption) {
    lines.push(['--capture FILE', 'write the last frame to a PNG file (with --frames)']);
    lines.push(['--capture-frames N,...', 'write those frames instead, to <FILE>/<frame>.png (FILE a directory)']);
  }
  lines.push(['--validation', 'accepted for the native programs\' scripts: WebGPU always validates']);
  const width = Math.max(...lines.map(([left]) => left.length));
  return [
    `usage: ${program.name}${positionals.map((p) => ` ${p.metavar}`).join('')} [options]`,
    ...(program.about ? [program.about] : []),
    ...lines.map(([left, right]) => `  ${left.padEnd(width + 2)}${right}`),
  ].join('\n') + '\n';
}

// The most recent frame's copy of a capture's readback, as RGBA8 pixels
export async function capturePixels(graph: RenderGraph, node: NodeId): Promise<{ pixels: Uint8Array; size: [number, number] }> {
  const readback = graph.node<ReadbackNode>(node);
  if (readback.source.format !== 'rgba8unorm' && readback.source.format !== 'rgba8unorm-srgb') {
    throw new Error('captures are of RGBA8 images (rgba8 or rgba8-srgb)');
  }
  const pixels = await readback.pixels();
  return { pixels, size: readback.size() };
}

// Runs a program where it is: under Node with its command line, or in a page with its query
// string's options. url: the generated main's (import.meta.url), which assets are found beside.
export async function runProgram<A extends RunnableApp, I>(program: Program<A, I>, url: string): Promise<void> {
  const node = (globalThis as { process?: { versions?: { node?: string } } }).process?.versions?.node;
  if (node) {
    const { runNode } = await import('./main-node.ts');
    await runNode(program, url);
  } else {
    const { runPage } = await import('./main-browser.ts');
    await runPage(program, url);
  }
}
