// Arithmetic with ZGAL's (and C++'s) semantics in JavaScript, whose numbers are doubles: floats are
// rounded to 32 bits after each operation, and ints and uints wrap at 32 bits. Operations take
// scalars or vectors (arrays), a scalar with a vector applying to each component.

export type Kind = 'float' | 'int' | 'uint';
export type Num = number | number[];

export function wrap(kind: Kind, x: number): number {
  return kind === 'float' ? Math.fround(x) : kind === 'int' ? x | 0 : x >>> 0;
}

function lift(a: Num, b: Num, f: (x: number, y: number) => number): Num {
  if (Array.isArray(a)) return a.map((x, i) => f(x, Array.isArray(b) ? b[i] : b));
  if (Array.isArray(b)) return b.map((y) => f(a, y));
  return f(a, b);
}

function each(a: Num, f: (x: number) => number): Num {
  return Array.isArray(a) ? a.map(f) : f(a);
}

export function add(kind: Kind, a: Num, b: Num): Num {
  return lift(a, b, (x, y) => wrap(kind, x + y));
}

export function sub(kind: Kind, a: Num, b: Num): Num {
  return lift(a, b, (x, y) => wrap(kind, x - y));
}

export function mul(kind: Kind, a: Num, b: Num): Num {
  return lift(a, b, (x, y) => (kind === 'float' ? Math.fround(x * y) : wrap(kind, Math.imul(x, y))));
}

// Integer division truncates (C++'s; a zero divisor gives 0 rather than being undefined)
export function div(kind: Kind, a: Num, b: Num): Num {
  return lift(a, b, (x, y) => (kind === 'float' ? Math.fround(x / y) : y === 0 ? 0 : wrap(kind, Math.trunc(x / y))));
}

export function rem(kind: Kind, a: Num, b: Num): Num {
  return lift(a, b, (x, y) => (y === 0 ? 0 : wrap(kind, x % y)));
}

export function neg(kind: Kind, a: Num): Num {
  return each(a, (x) => wrap(kind, -x));
}

export function min(a: Num, b: Num): Num {
  return lift(a, b, Math.min);
}

export function max(a: Num, b: Num): Num {
  return lift(a, b, Math.max);
}

// Conversions, as C++'s static_cast: floats to integers truncate
export function toFloat(x: number): number {
  return Math.fround(x);
}

export function toInt(x: number): number {
  return Math.trunc(x) | 0;
}

export function toUint(x: number): number {
  return Math.trunc(x) >>> 0;
}

// An element of data, checked to exist
export function elementOf<T>(data: readonly T[], index: number, what: string): T {
  if (index < 0 || index >= data.length) throw new Error(`${what}: element ${index} of ${data.length}`);
  return data[index];
}
