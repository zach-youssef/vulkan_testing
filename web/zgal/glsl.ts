// ZGAL's built-in functions (GLSL's) on the CPU, as GLM computes them for generated C++: floats are
// rounded to 32 bits after each step, and functions of scalars apply to each component of vectors.

type V = number | number[];
const f = Math.fround;

// g applied to each component of its arguments, a scalar argument going with every component
function map(g: (...xs: number[]) => number, ...args: V[]): V {
  const n = args.find(Array.isArray)?.length;
  if (n === undefined) return g(...(args as number[]));
  return Array.from({ length: n }, (_, i) => g(...args.map((a) => (Array.isArray(a) ? a[i] : a))));
}

const unary = (g: (x: number) => number) => <T extends V>(x: T): T => map((a) => g(a), x) as T;

export const radians = unary((x) => f(x * f(0.017453292519943295)));
export const degrees = unary((x) => f(x * f(57.29577951308232)));
export const sin = unary((x) => f(Math.sin(x)));
export const cos = unary((x) => f(Math.cos(x)));
export const tan = unary((x) => f(Math.tan(x)));
export const asin = unary((x) => f(Math.asin(x)));
export const acos = unary((x) => f(Math.acos(x)));
export const sinh = unary((x) => f(Math.sinh(x)));
export const cosh = unary((x) => f(Math.cosh(x)));
export const tanh = unary((x) => f(Math.tanh(x)));
export const asinh = unary((x) => f(Math.asinh(x)));
export const acosh = unary((x) => f(Math.acosh(x)));
export const atanh = unary((x) => f(Math.atanh(x)));
export const exp = unary((x) => f(Math.exp(x)));
export const log = unary((x) => f(Math.log(x)));
export const exp2 = unary((x) => f(2 ** x));
export const log2 = unary((x) => f(Math.log2(x)));
export const sqrt = unary((x) => f(Math.sqrt(x)));
export const inversesqrt = unary((x) => f(1 / f(Math.sqrt(x))));
export const floor = unary(Math.floor);
export const ceil = unary(Math.ceil);
export const trunc = unary(Math.trunc);
// Halves away from zero (std::round), and to even
export const round = unary((x) => Math.sign(x) * Math.round(Math.abs(x)));
export const roundEven = unary((x) => {
  const r = Math.round(x);
  return r - x === 0.5 && r % 2 !== 0 ? r - 1 : r;
});
export const fract = unary((x) => f(x - Math.floor(x)));
export const abs = unary(Math.abs);
export const sign = unary((x) => (x > 0 ? 1 : x < 0 ? -1 : 0));

export function atan<T extends V>(y: T, x?: T): T {
  return (x === undefined ? map((a) => f(Math.atan(a)), y) : map((a, b) => f(Math.atan2(a, b)), y, x)) as T;
}

export function pow<T extends V>(x: T, y: T): T {
  return map((a, b) => f(a ** b), x, y) as T;
}

export function mod<T extends V>(x: T, y: V): T {
  return map((a, b) => f(a - f(b * Math.floor(f(a / b)))), x, y) as T;
}

export function min<T extends V>(x: T, y: V): T {
  return map(Math.min, x, y) as T;
}

export function max<T extends V>(x: T, y: V): T {
  return map(Math.max, x, y) as T;
}

export function clamp<T extends V>(x: T, lo: V, hi: V): T {
  return map((a, l, h) => Math.min(Math.max(a, l), h), x, lo, hi) as T;
}

// x * (1 - a) + y * a; a bool vector chooses components instead
export function mix<T extends V>(x: T, y: T, a: V | boolean[]): T {
  if (Array.isArray(a) && typeof a[0] === 'boolean') return (x as number[]).map((xi, i) => ((a as boolean[])[i] ? (y as number[])[i] : xi)) as T;
  return map((xi, yi, ai) => f(f(xi * f(1 - ai)) + f(yi * ai)), x, y, a as V) as T;
}

export function step<T extends V>(edge: V, x: T): T {
  return map((e, a) => (a < e ? 0 : 1), edge, x) as T;
}

export function smoothstep<T extends V>(edge0: V, edge1: V, x: T): T {
  return map((e0, e1, a) => {
    const t = Math.min(Math.max(f(f(a - e0) / f(e1 - e0)), 0), 1);
    return f(f(t * t) * f(3 - f(2 * t)));
  }, edge0, edge1, x) as T;
}

export function dot(x: V, y: V): number {
  if (!Array.isArray(x) || !Array.isArray(y)) return f((x as number) * (y as number));
  let sum = f(x[0] * y[0]);
  for (let i = 1; i < x.length; i++) sum = f(sum + f(x[i] * y[i]));
  return sum;
}

export function length(x: V): number {
  return f(Math.sqrt(dot(x, x)));
}

export function distance(x: V, y: V): number {
  return length(map((a, b) => f(a - b), x, y));
}

export function normalize<T extends V>(x: T): T {
  const s = inversesqrt(dot(x, x));
  return map((a) => f(a * s), x) as T;
}

export function cross(x: number[], y: number[]): number[] {
  return [
    f(f(x[1] * y[2]) - f(y[1] * x[2])),
    f(f(x[2] * y[0]) - f(y[2] * x[0])),
    f(f(x[0] * y[1]) - f(y[0] * x[1])),
  ];
}

// I - N * dot(N, I) * 2
export function reflect<T extends V>(i: T, n: T): T {
  const d = f(dot(n, i) * 2);
  return map((a, b) => f(a - f(b * d)), i, n) as T;
}

export function refract<T extends V>(i: T, n: T, eta: number): T {
  const d = dot(n, i);
  const k = f(1 - f(f(eta * eta) * f(1 - f(d * d))));
  if (k < 0) return map(() => 0, i) as T;
  const s = f(f(eta * d) + f(Math.sqrt(k)));
  return map((a, b) => f(f(eta * a) - f(s * b)), i, n) as T;
}

export function faceforward<T extends V>(n: T, i: T, nref: T): T {
  return dot(nref, i) < 0 ? n : (map((a) => -a, n) as T);
}

const compare = (g: (a: number, b: number) => boolean) => (x: number[], y: number[]): boolean[] => x.map((a, i) => g(a, y[i]));
export const lessThan = compare((a, b) => a < b);
export const lessThanEqual = compare((a, b) => a <= b);
export const greaterThan = compare((a, b) => a > b);
export const greaterThanEqual = compare((a, b) => a >= b);
export const equal = <T>(x: T[], y: T[]): boolean[] => x.map((a, i) => a === y[i]);
export const notEqual = <T>(x: T[], y: T[]): boolean[] => x.map((a, i) => a !== y[i]);
export const any = (x: boolean[]): boolean => x.some((a) => a);
export const all = (x: boolean[]): boolean => x.every((a) => a);

// Matrices (arrays of columns), computed in doubles and rounded
export function transpose(m: number[][]): number[][] {
  return m[0].map((_, r) => m.map((column) => column[r]));
}

export function determinant(m: number[][]): number {
  return f(det(m));
}

function det(m: number[][]): number {
  if (m.length === 1) return m[0][0];
  if (m.length === 2) return m[0][0] * m[1][1] - m[1][0] * m[0][1];
  return m[0].reduce((sum, a, r) => sum + (r % 2 === 0 ? 1 : -1) * a * det(minor(m, 0, r)), 0);
}

// m without column c and row r
function minor(m: number[][], c: number, r: number): number[][] {
  return m.filter((_, i) => i !== c).map((column) => column.filter((_, j) => j !== r));
}

export function inverse(m: number[][]): number[][] {
  const d = det(m);
  const n = m.length;
  if (n === 1) return [[f(1 / d)]];
  // The adjugate over the determinant: element (column i, row j) is the cofactor of (column j, row i)
  return Array.from({ length: n }, (_, i) =>
    Array.from({ length: n }, (_, j) => f((((i + j) % 2 === 0 ? 1 : -1) * det(minor(m, j, i))) / d)));
}

// Bit casts between floats and 32-bit integers
const bits = new DataView(new ArrayBuffer(4));
export const floatBitsToUint = unary((x) => (bits.setFloat32(0, x), bits.getUint32(0)));
export const floatBitsToInt = unary((x) => (bits.setFloat32(0, x), bits.getInt32(0)));
export const uintBitsToFloat = unary((x) => (bits.setUint32(0, x), bits.getFloat32(0)));
export const intBitsToFloat = unary((x) => (bits.setInt32(0, x), bits.getFloat32(0)));
