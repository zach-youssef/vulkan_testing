// Arithmetic with ZGAL's (and C++'s) semantics in JavaScript, whose numbers are doubles: floats are
// rounded to 32 bits after each operation, and ints and uints wrap at 32 bits. Values are numbers
// (scalars), arrays of numbers (vectors) and arrays of columns (matrices); a scalar with a vector or
// matrix applies to each component, as GLM's operators do.

export type Kind = 'float' | 'int' | 'uint';
export type Num = number | number[];
// A scalar, vector or matrix
export type Value = number | boolean | Value[];

export function wrap(kind: Kind, x: number): number {
  return kind === 'float' ? Math.fround(x) : kind === 'int' ? x | 0 : x >>> 0;
}

// f applied to the components of a and b, which may be scalars, vectors or matrices
function lift<T extends Value>(a: Value, b: Value, f: (x: number, y: number) => number): T {
  if (Array.isArray(a)) return a.map((x, i) => lift(x, Array.isArray(b) ? b[i] : b, f)) as T;
  if (Array.isArray(b)) return b.map((y) => lift(a, y, f)) as T;
  return f(a as number, b as number) as T;
}

function each<T extends Value>(a: Value, f: (x: number) => number): T {
  return (Array.isArray(a) ? a.map((x) => each(x, f)) : f(a as number)) as T;
}

const isMatrix = (v: Value): v is number[][] => Array.isArray(v) && Array.isArray(v[0]);

export function add<T extends Value>(kind: Kind, a: Value, b: Value): T {
  return lift(a, b, (x, y) => wrap(kind, x + y));
}

export function sub<T extends Value>(kind: Kind, a: Value, b: Value): T {
  return lift(a, b, (x, y) => wrap(kind, x - y));
}

function scalarMul(kind: Kind, x: number, y: number): number {
  return kind === 'float' ? Math.fround(x * y) : wrap(kind, Math.imul(x, y));
}

// The sum of products a[i] * b[i] in order, rounding each step as C++ does
function dotProduct(a: number[], b: number[]): number {
  let sum = Math.fround(a[0] * b[0]);
  for (let i = 1; i < a.length; i++) sum = Math.fround(sum + Math.fround(a[i] * b[i]));
  return sum;
}

// Products of matrices (columns) with matrices and vectors are linear algebra; others are
// component-wise
export function mul<T extends Value>(kind: Kind, a: Value, b: Value): T {
  if (isMatrix(a) && isMatrix(b)) return b.map((column) => matrixTimesVector(a, column)) as T;
  if (isMatrix(a) && Array.isArray(b)) return matrixTimesVector(a, b as number[]) as T;
  if (Array.isArray(a) && isMatrix(b)) return b.map((column) => dotProduct(a as number[], column)) as T;
  return lift(a, b, (x, y) => scalarMul(kind, x, y));
}

function matrixTimesVector(m: number[][], v: number[]): number[] {
  return m[0].map((_, row) => dotProduct(m.map((column) => column[row]), v));
}

// Integer division truncates (C++'s; a zero divisor gives 0 rather than being undefined)
export function div<T extends Value>(kind: Kind, a: Value, b: Value): T {
  return lift(a, b, (x, y) => (kind === 'float' ? Math.fround(x / y) : y === 0 ? 0 : wrap(kind, Math.trunc(x / y))));
}

export function rem<T extends Value>(kind: Kind, a: Value, b: Value): T {
  return lift(a, b, (x, y) => (y === 0 ? 0 : wrap(kind, x % y)));
}

export function neg<T extends Value>(kind: Kind, a: Value): T {
  return each(a, (x) => wrap(kind, -x));
}

// Bitwise operations on ints and uints, and their vectors
export function bitand<T extends Value>(kind: Kind, a: Value, b: Value): T {
  return lift(a, b, (x, y) => wrap(kind, x & y));
}

export function bitor<T extends Value>(kind: Kind, a: Value, b: Value): T {
  return lift(a, b, (x, y) => wrap(kind, x | y));
}

export function bitxor<T extends Value>(kind: Kind, a: Value, b: Value): T {
  return lift(a, b, (x, y) => wrap(kind, x ^ y));
}

export function shl<T extends Value>(kind: Kind, a: Value, b: Value): T {
  return lift(a, b, (x, y) => wrap(kind, x << y));
}

export function shr<T extends Value>(kind: Kind, a: Value, b: Value): T {
  return lift(a, b, (x, y) => (kind === 'uint' ? x >>> y : x >> y));
}

export function bitnot<T extends Value>(kind: Kind, a: Value): T {
  return each(a, (x) => wrap(kind, ~x));
}

export function min<T extends Value>(a: Value, b: Value): T {
  return lift(a, b, Math.min);
}

export function max<T extends Value>(a: Value, b: Value): T {
  return lift(a, b, Math.max);
}

// Whether two values are equal, component by component
export function equal(a: Value, b: Value): boolean {
  if (Array.isArray(a) && Array.isArray(b)) return a.length === b.length && a.every((x, i) => equal(x, b[i]));
  return a === b;
}

// Conversions, as C++'s static_cast: floats to integers truncate, and bools are 0 or 1
export function toFloat(x: number | boolean): number {
  return Math.fround(Number(x));
}

export function toInt(x: number | boolean): number {
  return Math.trunc(Number(x)) | 0;
}

export function toUint(x: number | boolean): number {
  return Math.trunc(Number(x)) >>> 0;
}

export function toBool(x: number | boolean): boolean {
  return Number(x) !== 0;
}

export type Small = 'uint8' | 'int8' | 'uint16' | 'int16';
export type Component = Kind | 'bool' | Small;

// x narrowed to a small integer, wrapping
export function narrow(k: Small, x: number | boolean): number {
  switch (k) {
    case 'uint8': return toUint(x) & 0xff;
    case 'uint16': return toUint(x) & 0xffff;
    case 'int8': return (toInt(x) << 24) >> 24;
    case 'int16': return (toInt(x) << 16) >> 16;
  }
}

// x converted to a component of kind k
export function convert(k: Component, x: number | boolean): number | boolean {
  switch (k) {
    case 'float': return toFloat(x);
    case 'int': return toInt(x);
    case 'uint': return toUint(x);
    case 'bool': return toBool(x);
    default: return narrow(k, x);
  }
}

function components(args: Value[]): (number | boolean)[] {
  return args.flatMap((a) => (Array.isArray(a) ? components(a) : [a]));
}

// A vector of n components of kind k, as GLSL's constructors make them: from one scalar (every
// component), from a longer vector (its first n), or from scalars and vectors giving n in all
export function vector<T extends Value>(k: Component, n: number, ...args: Value[]): T {
  const parts = args.length === 1 && !Array.isArray(args[0]) ? Array<Value>(n).fill(args[0]) : components(args).slice(0, n);
  return parts.map((x) => convert(k, x as number | boolean)) as T;
}

// An n by n float matrix, as GLSL's constructors make them: from a scalar (on the diagonal), from
// another matrix (its top left, the identity elsewhere), or from columns' components in order
export function matrix(n: number, ...args: Value[]): number[][] {
  const identity = (i: number, j: number) => (i === j ? 1 : 0);
  if (args.length === 1 && !Array.isArray(args[0])) {
    const d = toFloat(args[0] as number);
    return Array.from({ length: n }, (_, i) => Array.from({ length: n }, (_, j) => (i === j ? d : 0)));
  }
  if (args.length === 1 && isMatrix(args[0])) {
    const m = args[0];
    return Array.from({ length: n }, (_, i) => Array.from({ length: n }, (_, j) => (i < m.length && j < m.length ? m[i][j] : identity(i, j))));
  }
  const flat = components(args).map((x) => toFloat(x as number));
  return Array.from({ length: n }, (_, i) => flat.slice(i * n, (i + 1) * n));
}

// The components of v at indices, as a vector (a swizzle)
export function swizzle<T>(v: readonly T[], indices: number[]): T[] {
  return indices.map((i) => v[i]);
}

// An element of data, checked to exist
export function elementOf<T>(data: readonly T[], index: number, what: string): T {
  if (index < 0 || index >= data.length) throw new Error(`${what}: element ${index} of ${data.length}`);
  return data[index];
}

// A float as C++'s streams print it by default (printf's %g: 6 significant digits)
export function formatFloat(x: number): string {
  if (!Number.isFinite(x)) return Number.isNaN(x) ? 'nan' : x > 0 ? 'inf' : '-inf';
  if (x === 0) return Object.is(x, -0) ? '-0' : '0';
  const exponent = Number(x.toExponential(5).split('e')[1]);
  if (exponent < -4 || exponent >= 6) {
    const [mantissa, e] = x.toExponential(5).split('e');
    const sign = e[0] === '-' ? '-' : '+';
    const digits = e.replace(/^[-+]/, '').padStart(2, '0');
    return `${stripZeros(mantissa)}e${sign}${digits}`;
  }
  return stripZeros(x.toFixed(Math.max(0, 5 - exponent)));
}

function stripZeros(s: string): string {
  return s.includes('.') ? s.replace(/0+$/, '').replace(/\.$/, '') : s;
}
