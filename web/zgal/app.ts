// What the TypeScript ZGAL generates for apps uses at runtime: the events an app handles, and the
// clock its frame handler runs on (include/zgal/App.h's, for the web). Free of the DOM: a page
// connects an app to a canvas's events, and headless code calls the app's beginFrame() itself.

// The keys apps can handle (ZGAL's key names: a-z, 0-9, space, escape, ...), named as C++'s zgal::Key
export type Key =
  | 'A' | 'B' | 'C' | 'D' | 'E' | 'F' | 'G' | 'H' | 'I' | 'J' | 'K' | 'L' | 'M' | 'N' | 'O' | 'P' | 'Q' | 'R'
  | 'S' | 'T' | 'U' | 'V' | 'W' | 'X' | 'Y' | 'Z'
  | 'Digit0' | 'Digit1' | 'Digit2' | 'Digit3' | 'Digit4' | 'Digit5' | 'Digit6' | 'Digit7' | 'Digit8' | 'Digit9'
  | 'Space' | 'Escape' | 'Enter' | 'Tab' | 'Backspace' | 'Left' | 'Right' | 'Up' | 'Down' | 'Minus' | 'Equal'
  | 'KeypadAdd' | 'KeypadSubtract'
  | 'F1' | 'F2' | 'F3' | 'F4' | 'F5' | 'F6' | 'F7' | 'F8' | 'F9' | 'F10' | 'F11' | 'F12';

export type MouseButton = 'left' | 'right' | 'middle';

// Cursor positions are in [-1, 1] across the window, y down, like clip space; scroll offsets are in
// steps (lines), y up
export type Event =
  | { kind: 'key-pressed'; key: Key }
  | { kind: 'key-released'; key: Key }
  | { kind: 'mouse-pressed'; button: MouseButton; cursor: [number, number] }
  | { kind: 'mouse-released'; button: MouseButton; cursor: [number, number] }
  | { kind: 'cursor-moved'; cursor: [number, number] }
  | { kind: 'scrolled'; offset: [number, number] };

// How far an app's clock advances each frame: with real time (the time since the last frame, at most
// maxStep, so a stall doesn't make the app jump; 0 on the first frame), or by a fixed step, which
// makes headless runs deterministic. A required choice: apps take one in their inputs.
export class Timestep {
  private readonly realTime: boolean;
  private readonly step: number;
  private last: number | null = null;

  private constructor(realTime: boolean, step: number) {
    this.realTime = realTime;
    this.step = Math.fround(step);
  }

  static realTime(maxStep: number): Timestep {
    return new Timestep(true, maxStep);
  }

  static fixed(step: number): Timestep {
    return new Timestep(false, step);
  }

  // The time, in seconds, since the previous frame
  next(): number {
    if (!this.realTime) return this.step;
    const now = performance.now();
    const dt = this.last === null ? 0 : Math.min(Math.fround((now - this.last) / 1000), this.step);
    this.last = now;
    return dt;
  }
}

// How an app is run, which its bodies read as headless, capture and frameLimit: in a window, or
// headless (drawing into images, which a capture reads back), for a number of frames (0: until it
// quits or the page closes)
export class RunMode {
  readonly headless: boolean;
  readonly capture: boolean;
  readonly frameLimit: number;

  private constructor(headless: boolean, capture: boolean, frameLimit: number) {
    this.headless = headless;
    this.capture = capture;
    this.frameLimit = frameLimit;
  }

  static window(frameLimit: number): RunMode {
    return new RunMode(false, false, frameLimit);
  }

  static headless(frameLimit: number, capture: boolean): RunMode {
    return new RunMode(true, capture, frameLimit);
  }
}

// What print! writes, a line at a time (console.log unless replaced, e.g. by tests)
export const output = {
  line: (text: string): void => console.log(text),
};
