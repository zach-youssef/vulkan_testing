// ZGAL's types at runtime: how values of each type are laid out in buffers (std430, as the C++ and
// the shaders lay them out), so generated code can write and read structs, vectors and arrays.
//
// Values are plain JavaScript: numbers for scalars (booleans for bools), arrays of numbers for
// vectors, arrays of columns for matrices, arrays for arrays, and objects for structs.

export type Scalar = number;
export type Vector = number[];
export type Matrix = number[][];

export type ScalarKind = 'float' | 'int' | 'uint' | 'bool' | 'uint8' | 'int8' | 'uint16' | 'int16';

export interface Type<T = unknown> {
  readonly name: string;
  // std430 size and alignment in bytes
  readonly size: number;
  readonly align: number;
  write(view: DataView, offset: number, value: T): void;
  read(view: DataView, offset: number): T;
}

export function stride(t: Type): number {
  return Math.ceil(t.size / t.align) * t.align;
}

function scalar(kind: ScalarKind): Type<number> {
  const size = kind === 'uint8' || kind === 'int8' ? 1 : kind === 'uint16' || kind === 'int16' ? 2 : 4;
  const write = {
    float: (v: DataView, o: number, x: number) => v.setFloat32(o, x, true),
    int: (v: DataView, o: number, x: number) => v.setInt32(o, x, true),
    uint: (v: DataView, o: number, x: number) => v.setUint32(o, x >>> 0, true),
    bool: (v: DataView, o: number, x: number) => v.setUint32(o, x ? 1 : 0, true),
    uint8: (v: DataView, o: number, x: number) => v.setUint8(o, x),
    int8: (v: DataView, o: number, x: number) => v.setInt8(o, x),
    uint16: (v: DataView, o: number, x: number) => v.setUint16(o, x, true),
    int16: (v: DataView, o: number, x: number) => v.setInt16(o, x, true),
  }[kind];
  const read = {
    float: (v: DataView, o: number) => v.getFloat32(o, true),
    int: (v: DataView, o: number) => v.getInt32(o, true),
    uint: (v: DataView, o: number) => v.getUint32(o, true),
    bool: (v: DataView, o: number) => v.getUint32(o, true),
    uint8: (v: DataView, o: number) => v.getUint8(o),
    int8: (v: DataView, o: number) => v.getInt8(o),
    uint16: (v: DataView, o: number) => v.getUint16(o, true),
    int16: (v: DataView, o: number) => v.getInt16(o, true),
  }[kind];
  return { name: kind, size, align: size, write, read };
}

export const float = scalar('float');
export const int = scalar('int');
export const uint = scalar('uint');
export const uint8 = scalar('uint8');
export const int8 = scalar('int8');
export const uint16 = scalar('uint16');
export const int16 = scalar('int16');
// Bools in buffers are 32-bit (ZGAL's buffers don't hold them, but per-frame values may be bools)
export const bool: Type<boolean> = {
  name: 'bool',
  size: 4,
  align: 4,
  write: (v, o, x) => v.setUint32(o, x ? 1 : 0, true),
  read: (v, o) => v.getUint32(o, true) !== 0,
};

export function vector(component: Type<number>, n: 2 | 3 | 4): Type<Vector> {
  return {
    name: `${component.name}${n}`,
    size: 4 * n,
    align: n === 2 ? 8 : 16,
    write(view, offset, value) {
      for (let i = 0; i < n; i++) component.write(view, offset + 4 * i, value[i]);
    },
    read(view, offset) {
      return Array.from({ length: n }, (_, i) => component.read(view, offset + 4 * i));
    },
  };
}

export function matrix(n: 2 | 3 | 4): Type<Matrix> {
  const column = vector(float, n);
  const columnStride = stride(column);
  return {
    name: `mat${n}`,
    size: n * columnStride,
    align: column.align,
    write(view, offset, value) {
      for (let c = 0; c < n; c++) column.write(view, offset + c * columnStride, value[c]);
    },
    read(view, offset) {
      return Array.from({ length: n }, (_, c) => column.read(view, offset + c * columnStride));
    },
  };
}

export const vec2 = vector(float, 2);
export const vec3 = vector(float, 3);
export const vec4 = vector(float, 4);
export const ivec2 = vector(int, 2);
export const ivec3 = vector(int, 3);
export const ivec4 = vector(int, 4);
export const uvec2 = vector(uint, 2);
export const uvec3 = vector(uint, 3);
export const uvec4 = vector(uint, 4);
export const mat2 = matrix(2);
export const mat3 = matrix(3);
export const mat4 = matrix(4);

export function array<T>(element: Type<T>, length: number): Type<T[]> {
  const elementStride = stride(element);
  return {
    name: `(array ${element.name} ${length})`,
    size: length * elementStride,
    align: element.align,
    write(view, offset, value) {
      for (let i = 0; i < length; i++) element.write(view, offset + i * elementStride, value[i]);
    },
    read(view, offset) {
      return Array.from({ length }, (_, i) => element.read(view, offset + i * elementStride));
    },
  };
}

// A struct of fields, in order, at their std430 offsets
export function struct<T extends object>(name: string, fields: [keyof T & string, Type<any>][]): Type<T> {
  let offset = 0;
  let align = 1;
  const placed = fields.map(([field, type]) => {
    offset = Math.ceil(offset / type.align) * type.align;
    const at = offset;
    offset += type.size;
    align = Math.max(align, type.align);
    return { field, type, at };
  });
  return {
    name,
    size: Math.ceil(offset / align) * align,
    align,
    write(view, base, value) {
      for (const f of placed) f.type.write(view, base + f.at, (value as Record<string, unknown>)[f.field]);
    },
    read(view, base) {
      const value: Record<string, unknown> = {};
      for (const f of placed) value[f.field] = f.type.read(view, base + f.at);
      return value as T;
    },
  };
}

// Elements of type t as bytes, one after another at its stride (a buffer's contents), the whole
// rounded up to 4 bytes, as WebGPU buffers are
export function encode<T>(t: Type<T>, elements: readonly T[]): Uint8Array<ArrayBuffer> {
  const s = stride(t);
  const bytes = new Uint8Array(Math.max(4, Math.ceil((s * elements.length) / 4) * 4));
  const view = new DataView(bytes.buffer);
  elements.forEach((e, i) => t.write(view, i * s, e));
  return bytes;
}

// The elements of type t in bytes (as many as fit)
export function decode<T>(t: Type<T>, bytes: ArrayBuffer | ArrayBufferView, count?: number): T[] {
  const view = ArrayBuffer.isView(bytes)
    ? new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
    : new DataView(bytes);
  const s = stride(t);
  const n = count ?? Math.floor(view.byteLength / s);
  return Array.from({ length: n }, (_, i) => t.read(view, i * s));
}
