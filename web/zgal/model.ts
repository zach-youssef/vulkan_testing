// Models loaded from files, as resources/Model.h loads them natively: glTF (.gltf, .glb) and OBJ
// (.obj, with its .mtl). ModelData is what a file holds: triangles with their transforms baked in
// and base colour materials (a factor and an optional texture), the file's then a default one.
// Texture coordinates have glTF's convention (v = 0 at the top of the image); OBJ's are flipped to
// it. Front faces are counter-clockwise.

import type { Context } from './context.ts';
import { createMesh, type Mesh, type RenderGraph } from './graph.ts';
import { decodeImage, writeImage } from './assets.ts';

// Triangles with one material. Attributes are per vertex, flat (3, 3, 2, 4 and 4 numbers a vertex);
// ones the file lacks are empty.
export interface ModelPrimitive {
  positions: number[];
  normals: number[];
  uvs: number[];          // glTF's TEXCOORD_0
  colors: number[];       // glTF's COLOR_0 (alpha 1 when it has three components)
  tangents: number[];     // xyz and the bitangent's sign
  indices: number[];      // Three per triangle
  material: number;       // Into ModelData's materials
}

// An image a material uses: an asset's path, or an encoded file's bytes (embedded in a glTF file)
export interface ModelImage {
  readonly path: string | null;
  readonly encoded: Uint8Array | null;
  readonly description: string;    // For errors, e.g. "image 2 of 'ship.gltf'"
}

export interface ModelMaterial {
  readonly name: string;
  readonly baseColorFactor: [number, number, number, number];   // Linear
  readonly baseColorTexture: ModelImage | null;                 // sRGB
}

export interface ModelData {
  readonly primitives: ModelPrimitive[];
  // The file's materials, then a default one (white, untextured) for primitives without any
  readonly materials: ModelMaterial[];
}

const defaultMaterial = (): ModelMaterial => ({ name: 'default', baseColorFactor: [1, 1, 1, 1], baseColorTexture: null });

// Loads a .gltf, .glb or .obj asset (by extension)
export async function loadModelData(context: Context, path: string): Promise<ModelData> {
  const extension = /\.[^./]*$/.exec(path)?.[0].toLowerCase();
  let model: ModelData;
  if (extension === '.obj') model = await loadObj(context, path);
  else if (extension === '.gltf' || extension === '.glb') model = await loadGltf(context, path);
  else throw new Error(`Model '${path}' isn't a .gltf, .glb or .obj file`);
  if (model.primitives.length === 0) throw new Error(`Model '${path}' has no triangles`);
  return model;
}

// A path relative to another asset's directory
function beside(path: string, relative: string): string {
  const slash = path.lastIndexOf('/');
  return slash < 0 ? relative : path.slice(0, slash + 1) + relative;
}

function decodeBase64(text: string): Uint8Array {
  const binary = atob(text);
  const bytes = new Uint8Array(binary.length);
  for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
  return bytes;
}

// ---------------------------------------------------------------------------------------------
// glTF

// Column-major 4x4 matrices
type Mat4 = number[];
const identity = (): Mat4 => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

function multiply(a: Mat4, b: Mat4): Mat4 {
  const m = new Array<number>(16).fill(0);
  for (let c = 0; c < 4; c++) {
    for (let r = 0; r < 4; r++) {
      for (let k = 0; k < 4; k++) m[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
    }
  }
  return m;
}

// A node's local transform: its matrix, or translation * rotation * scale
function localTransform(node: GltfNode): Mat4 {
  if (node.matrix) return [...node.matrix];
  const [tx, ty, tz] = node.translation ?? [0, 0, 0];
  const [x, y, z, w] = node.rotation ?? [0, 0, 0, 1];
  const [sx, sy, sz] = node.scale ?? [1, 1, 1];
  return [
    (1 - 2 * (y * y + z * z)) * sx, 2 * (x * y + z * w) * sx, 2 * (x * z - y * w) * sx, 0,
    2 * (x * y - z * w) * sy, (1 - 2 * (x * x + z * z)) * sy, 2 * (y * z + x * w) * sy, 0,
    2 * (x * z + y * w) * sz, 2 * (y * z - x * w) * sz, (1 - 2 * (x * x + y * y)) * sz, 0,
    tx, ty, tz, 1,
  ];
}

// The upper 3x3 of m (column-major), its determinant, and the inverse transpose (normals' matrix)
function upper3(m: Mat4): number[] {
  return [m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]];
}
function determinant3(m: number[]): number {
  return m[0] * (m[4] * m[8] - m[7] * m[5]) - m[3] * (m[1] * m[8] - m[7] * m[2]) + m[6] * (m[1] * m[5] - m[4] * m[2]);
}
function normalMatrix(m: number[]): number[] {
  // The inverse transpose is the cofactor matrix over the determinant
  const d = determinant3(m);
  const at = (c: number, r: number) => m[c * 3 + r];
  const cofactor = (c: number, r: number) => {
    const cs = [0, 1, 2].filter((i) => i !== c);
    const rs = [0, 1, 2].filter((i) => i !== r);
    const minor = at(cs[0], rs[0]) * at(cs[1], rs[1]) - at(cs[1], rs[0]) * at(cs[0], rs[1]);
    return ((c + r) % 2 === 0 ? 1 : -1) * minor;
  };
  const result = new Array<number>(9);
  for (let c = 0; c < 3; c++) for (let r = 0; r < 3; r++) result[c * 3 + r] = cofactor(c, r) / d;
  return result;
}
const apply3 = (m: number[], v: number[]) => [0, 1, 2].map((r) => m[r] * v[0] + m[3 + r] * v[1] + m[6 + r] * v[2]);
function normalize(v: number[]): number[] {
  const length = Math.hypot(v[0], v[1], v[2]);
  return length > 0 ? v.map((x) => x / length) : [NaN, NaN, NaN];
}

interface GltfNode {
  name?: string;
  mesh?: number;
  children?: number[];
  matrix?: number[];
  translation?: number[];
  rotation?: number[];
  scale?: number[];
}
interface GltfAccessor {
  bufferView?: number;
  byteOffset?: number;
  componentType: number;
  normalized?: boolean;
  count: number;
  type: string;
  sparse?: {
    count: number;
    indices: { bufferView: number; byteOffset?: number; componentType: number };
    values: { bufferView: number; byteOffset?: number };
  };
}
interface GltfBufferView { buffer: number; byteOffset?: number; byteLength: number; byteStride?: number }
interface GltfPrimitive { attributes: Record<string, number>; indices?: number; material?: number; mode?: number; extensions?: Record<string, unknown> }
interface Gltf {
  scene?: number;
  scenes?: { nodes?: number[] }[];
  nodes?: GltfNode[];
  meshes?: { name?: string; primitives: GltfPrimitive[] }[];
  materials?: { name?: string; pbrMetallicRoughness?: { baseColorFactor?: number[]; baseColorTexture?: { index: number } } }[];
  textures?: { source?: number }[];
  images?: { uri?: string; bufferView?: number; mimeType?: string }[];
  buffers?: { uri?: string; byteLength: number }[];
  bufferViews?: GltfBufferView[];
  accessors?: GltfAccessor[];
}

const componentCounts: Record<string, number> = { SCALAR: 1, VEC2: 2, VEC3: 3, VEC4: 4 };
const componentSizes: Record<number, number> = { 5120: 1, 5121: 1, 5122: 2, 5123: 2, 5125: 4, 5126: 4 };

function readComponent(view: DataView, offset: number, type: number, normalized: boolean): number {
  switch (type) {
    case 5120: return normalized ? Math.max(view.getInt8(offset) / 127, -1) : view.getInt8(offset);
    case 5121: return normalized ? view.getUint8(offset) / 255 : view.getUint8(offset);
    case 5122: return normalized ? Math.max(view.getInt16(offset, true) / 32767, -1) : view.getInt16(offset, true);
    case 5123: return normalized ? view.getUint16(offset, true) / 65535 : view.getUint16(offset, true);
    case 5125: return view.getUint32(offset, true);
    case 5126: return view.getFloat32(offset, true);
  }
  throw new Error(`unknown component type ${type}`);
}

async function loadGltf(context: Context, path: string): Promise<ModelData> {
  const bytes = await context.loadAsset(path);
  const invalid = () => new Error(`glTF file '${path}' is invalid`);
  let gltf: Gltf;
  let binary: Uint8Array | null = null;
  try {
    const header = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    let json: Uint8Array = bytes;
    // A .glb: a header, then a JSON chunk and an optional binary one
    if (bytes.length >= 12 && header.getUint32(0, true) === 0x46546c67) {
      json = new Uint8Array(0);
      for (let at = 12; at + 8 <= bytes.length;) {
        const length = header.getUint32(at, true);
        const type = header.getUint32(at + 4, true);
        const chunk = bytes.subarray(at + 8, at + 8 + length);
        if (type === 0x4e4f534a) json = chunk;
        else if (type === 0x004e4942 && !binary) binary = chunk;
        at += 8 + length;
      }
    }
    gltf = JSON.parse(new TextDecoder().decode(json)) as Gltf;
  } catch {
    throw new Error(`Failed to parse glTF file '${path}'`);
  }

  let buffers: Uint8Array[];
  try {
    buffers = await Promise.all((gltf.buffers ?? []).map(async (b, i) => {
      let data: Uint8Array;
      if (b.uri === undefined) {
        if (i !== 0 || !binary) throw invalid();
        data = binary;
      } else if (b.uri.startsWith('data:')) {
        const comma = b.uri.indexOf(';base64,');
        if (comma < 0) throw invalid();
        data = decodeBase64(b.uri.slice(comma + 8));
      } else {
        data = await context.loadAsset(beside(path, decodeURIComponent(b.uri)));
      }
      if (data.length < b.byteLength) throw invalid();
      return data;
    }));
  } catch (error) {
    throw new Error(`Failed to load the buffers of glTF file '${path}': ${(error as Error).message}`);
  }

  const viewBytes = (index: number): [Uint8Array, number] => {
    const view = gltf.bufferViews?.[index];
    const buffer = view && buffers[view.buffer];
    if (!view || !buffer || (view.byteOffset ?? 0) + view.byteLength > buffer.length) throw invalid();
    return [buffer.subarray(view.byteOffset ?? 0, (view.byteOffset ?? 0) + view.byteLength), view.byteStride ?? 0];
  };
  // An accessor's elements, flattened to numbers (normalized integers as floats), and their size
  const accessor = (index: number): [number[], number] => {
    const a = gltf.accessors?.[index];
    const components = a && componentCounts[a.type];
    const size = a && componentSizes[a.componentType];
    if (!a || !components || !size) throw invalid();
    const values = new Array<number>(a.count * components).fill(0);
    const read = (bufferView: number, byteOffset: number, count: number, type: number, n: number, normalized: boolean, place: (i: number, c: number, v: number) => void) => {
      const [data, byteStride] = viewBytes(bufferView);
      const element = n * componentSizes[type];
      const step = byteStride || element;
      if (count > 0 && byteOffset + (count - 1) * step + element > data.length) throw invalid();
      const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
      for (let i = 0; i < count; i++) {
        for (let c = 0; c < n; c++) place(i, c, readComponent(view, byteOffset + i * step + c * componentSizes[type], type, normalized));
      }
    };
    const set = (i: number, c: number, v: number) => { values[i * components + c] = v; };
    if (a.bufferView !== undefined) read(a.bufferView, a.byteOffset ?? 0, a.count, a.componentType, components, a.normalized ?? false, set);
    if (a.sparse) {
      const s = a.sparse;
      const at: number[] = [];
      read(s.indices.bufferView, s.indices.byteOffset ?? 0, s.count, s.indices.componentType, 1, false, (_, __, v) => at.push(v));
      read(s.values.bufferView, s.values.byteOffset ?? 0, s.count, a.componentType, components, a.normalized ?? false, (i, c, v) => {
        if (at[i] >= a.count) throw invalid();
        set(at[i], c, v);
      });
    }
    return [values, components];
  };

  const image = (index: number): ModelImage => {
    const description = `image ${index} of '${path}'`;
    const source = gltf.images?.[index];
    if (!source) throw invalid();
    if (source.bufferView !== undefined) return { path: null, encoded: viewBytes(source.bufferView)[0], description };
    if (source.uri === undefined) throw new Error(`${description} has no data`);
    if (source.uri.startsWith('data:')) {
      const comma = source.uri.indexOf(';base64,');
      if (comma < 0) throw new Error(`${description} is a data URI that isn't base64`);
      return { path: null, encoded: decodeBase64(source.uri.slice(comma + 8)), description };
    }
    return { path: beside(path, decodeURIComponent(source.uri)), encoded: null, description };
  };

  const materials: ModelMaterial[] = (gltf.materials ?? []).map((m, i) => {
    const pbr = m.pbrMetallicRoughness;
    const f = pbr?.baseColorFactor ?? [1, 1, 1, 1];
    const texture = pbr?.baseColorTexture !== undefined ? gltf.textures?.[pbr.baseColorTexture.index] : undefined;
    return {
      name: m.name ?? `material ${i}`,
      baseColorFactor: [f[0], f[1], f[2], f[3]],
      baseColorTexture: texture?.source !== undefined ? image(texture.source) : null,
    };
  });
  const defaultIndex = materials.length;
  materials.push(defaultMaterial());

  // The default scene's nodes (or, without scenes, every node), with their world transforms
  const nodes = gltf.nodes ?? [];
  const parents = new Map<number, number>();
  nodes.forEach((n, i) => n.children?.forEach((c) => parents.set(c, i)));
  const world = (i: number): Mat4 => {
    const local = localTransform(nodes[i]);
    const parent = parents.get(i);
    return parent === undefined ? local : multiply(world(parent), local);
  };
  const order: number[] = [];
  const scene = gltf.scenes?.[gltf.scene ?? 0];
  if (scene) {
    const visit = (i: number) => {
      if (!nodes[i]) throw invalid();
      order.push(i);
      nodes[i].children?.forEach(visit);
    };
    scene.nodes?.forEach(visit);
  } else {
    nodes.forEach((_, i) => order.push(i));
  }

  const primitives: ModelPrimitive[] = [];
  for (const nodeIndex of order) {
    const node = nodes[nodeIndex];
    if (node.mesh === undefined) continue;
    const mesh = gltf.meshes?.[node.mesh];
    if (!mesh) throw invalid();
    const transform = world(nodeIndex);
    const linear = upper3(transform);
    const normals = normalMatrix(linear);
    // A mirroring transform turns counter-clockwise triangles clockwise
    const mirrored = determinant3(linear) < 0;
    mesh.primitives.forEach((source, p) => {
      const what = `primitive ${p} of mesh ${mesh.name ?? node.mesh} in '${path}'`;
      if (source.extensions && 'KHR_draco_mesh_compression' in source.extensions) throw new Error(`${what} is Draco compressed, which isn't supported`);
      if ((source.mode ?? 4) !== 4) throw new Error(`${what} isn't triangles (only triangle lists are supported)`);
      const primitive: ModelPrimitive = {
        positions: [], normals: [], uvs: [], colors: [], tangents: [], indices: [],
        material: source.material ?? defaultIndex,
      };
      if (source.material !== undefined && !gltf.materials?.[source.material]) throw invalid();
      for (const [name, index] of Object.entries(source.attributes)) {
        if (!['POSITION', 'NORMAL', 'TANGENT', 'TEXCOORD_0', 'COLOR_0'].includes(name)) continue;
        const [values, components] = accessor(index);
        const at = (i: number, c: number, otherwise: number) => (c < components ? values[i * components + c] : otherwise);
        for (let i = 0; i < values.length / components; i++) {
          switch (name) {
            case 'POSITION': {
              const v = [at(i, 0, 0), at(i, 1, 0), at(i, 2, 0)];
              primitive.positions.push(...[0, 1, 2].map((r) => transform[r] * v[0] + transform[4 + r] * v[1] + transform[8 + r] * v[2] + transform[12 + r]));
              break;
            }
            case 'NORMAL':
              primitive.normals.push(...normalize(apply3(normals, [at(i, 0, 0), at(i, 1, 0), at(i, 2, 0)])));
              break;
            case 'TANGENT':
              primitive.tangents.push(...normalize(apply3(linear, [at(i, 0, 0), at(i, 1, 0), at(i, 2, 0)])), at(i, 3, 1) * (mirrored ? -1 : 1));
              break;
            case 'TEXCOORD_0':
              primitive.uvs.push(at(i, 0, 0), at(i, 1, 0));
              break;
            case 'COLOR_0':
              primitive.colors.push(at(i, 0, 0), at(i, 1, 0), at(i, 2, 0), at(i, 3, 1));
              break;
          }
        }
      }
      if (primitive.positions.length === 0) throw new Error(`${what} has no positions`);
      const vertices = primitive.positions.length / 3;
      primitive.indices = source.indices !== undefined ? accessor(source.indices)[0] : Array.from({ length: vertices }, (_, i) => i);
      if (primitive.indices.some((i) => i >= vertices)) throw invalid();
      primitive.indices.length -= primitive.indices.length % 3;
      if (mirrored) {
        for (let i = 0; i < primitive.indices.length; i += 3) {
          [primitive.indices[i + 1], primitive.indices[i + 2]] = [primitive.indices[i + 2], primitive.indices[i + 1]];
        }
      }
      if (primitive.indices.length > 0) primitives.push(primitive);
    });
  }
  return { primitives, materials };
}

// ---------------------------------------------------------------------------------------------
// OBJ

// Materials of an .mtl file, by name
function parseMtl(text: string, mtlPath: string, materials: ModelMaterial[], ids: Map<string, number>): void {
  let current: { name: string; diffuse: number[]; dissolve: number; texture: string | null } | null = null;
  const finish = () => {
    if (!current) return;
    ids.set(current.name, materials.length);
    materials.push({
      name: current.name,
      baseColorFactor: [current.diffuse[0], current.diffuse[1], current.diffuse[2], current.dissolve],
      baseColorTexture: current.texture
        ? { path: beside(mtlPath, current.texture), encoded: null, description: `texture '${current.texture}' of '${mtlPath}'` }
        : null,
    });
  };
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.replace(/#.*/, '').trim();
    const [keyword, ...rest] = line.split(/\s+/);
    if (keyword === 'newmtl') {
      finish();
      current = { name: rest.join(' '), diffuse: [0, 0, 0], dissolve: 1, texture: null };
    } else if (!current) {
      continue;
    } else if (keyword === 'Kd') {
      current.diffuse = [0, 1, 2].map((i) => Number(rest[i] ?? rest[0]));
    } else if (keyword === 'd') {
      current.dissolve = Number(rest[0]);
    } else if (keyword === 'Tr') {
      current.dissolve = 1 - Number(rest[0]);
    } else if (keyword === 'map_Kd') {
      // Options (-o u v, ...) come first; the file name is last
      current.texture = rest[rest.length - 1] ?? null;
    }
  }
  finish();
}

async function loadObj(context: Context, path: string): Promise<ModelData> {
  const text = new TextDecoder().decode(await context.loadAsset(path));
  const positions: number[][] = [];
  const colors: (number[] | null)[] = [];
  const uvs: number[][] = [];
  const normals: number[][] = [];
  const materials: ModelMaterial[] = [];
  const materialIds = new Map<string, number>();
  type Corner = [number, number, number];   // Position, uv and normal indices (-1 for none)
  // Shapes ('o' and 'g' start one): their triangles, with each one's material (-1 for none)
  const shapes: { corners: Corner[]; materials: number[] }[] = [{ corners: [], materials: [] }];
  let material = -1;
  const lines = text.split(/\r?\n/);
  for (const [number, raw] of lines.entries()) {
    const line = raw.replace(/#.*/, '').trim();
    if (line === '') continue;
    const [keyword, ...rest] = line.split(/\s+/);
    const fail = (why: string) => new Error(`OBJ file '${path}': line ${number + 1}: ${why}`);
    switch (keyword) {
      case 'v':
        positions.push(rest.slice(0, 3).map(Number));
        colors.push(rest.length >= 6 ? rest.slice(3, 6).map(Number) : null);
        break;
      case 'vt':
        uvs.push([Number(rest[0]), Number(rest[1] ?? 0)]);
        break;
      case 'vn':
        normals.push(rest.slice(0, 3).map(Number));
        break;
      case 'f': {
        const index = (text: string | undefined, count: number) => {
          if (text === undefined || text === '') return -1;
          const i = Number(text);
          if (!Number.isInteger(i) || i === 0) throw fail(`invalid index '${text}'`);
          const at = i > 0 ? i - 1 : count + i;
          if (at < 0 || at >= count) throw fail(`index ${text} is out of range`);
          return at;
        };
        const corners: Corner[] = rest.map((c) => {
          const [v, t, n] = c.split('/');
          return [index(v, positions.length), index(t, uvs.length), index(n, normals.length)];
        });
        if (corners.length < 3) throw fail('a face has fewer than three corners');
        const shape = shapes[shapes.length - 1];
        const triangle = (a: number, b: number, c: number) => {
          shape.corners.push(corners[a], corners[b], corners[c]);
          shape.materials.push(material);
        };
        if (corners.length === 4) {
          // As tinyobjloader splits quads: along the shorter diagonal
          const distance = (a: number, b: number) => positions[corners[a][0]].reduce((s, x, k) => s + (positions[corners[b][0]][k] - x) ** 2, 0);
          if (distance(0, 2) < distance(1, 3)) {
            triangle(0, 1, 2);
            triangle(0, 2, 3);
          } else {
            triangle(0, 1, 3);
            triangle(1, 2, 3);
          }
        } else {
          for (let k = 1; k + 1 < corners.length; k++) triangle(0, k, k + 1);
        }
        break;
      }
      case 'o':
      case 'g':
        shapes.push({ corners: [], materials: [] });
        break;
      case 'usemtl':
        material = materialIds.get(rest.join(' ')) ?? -1;
        break;
      case 'mtllib':
        for (const file of rest) {
          const mtlPath = beside(path, file);
          let mtl: Uint8Array;
          try {
            mtl = await context.loadAsset(mtlPath);
          } catch (error) {
            throw new Error(`OBJ file '${path}': ${(error as Error).message}`);
          }
          parseMtl(new TextDecoder().decode(mtl), mtlPath, materials, materialIds);
        }
        break;
    }
  }
  const defaultIndex = materials.length;
  materials.push(defaultMaterial());

  // A primitive per shape and material (in the materials' order). OBJ indexes each attribute
  // separately, so vertices are the distinct combinations.
  const hasColors = colors.length > 0 && colors.every((c) => c !== null);
  const primitives: ModelPrimitive[] = [];
  for (const shape of shapes) {
    const byMaterial = new Map<number, { primitive: ModelPrimitive; ids: Map<string, number>; uvs: number; normals: number }>();
    shape.corners.forEach((corner, k) => {
      const id = shape.materials[Math.floor(k / 3)];
      const m = id >= 0 ? id : defaultIndex;
      let entry = byMaterial.get(m);
      if (!entry) {
        entry = { primitive: { positions: [], normals: [], uvs: [], colors: [], tangents: [], indices: [], material: m }, ids: new Map(), uvs: 0, normals: 0 };
        byMaterial.set(m, entry);
      }
      const { primitive, ids } = entry;
      const key = corner.join('/');
      let vertex = ids.get(key);
      if (vertex === undefined) {
        vertex = primitive.positions.length / 3;
        const [v, t, n] = corner;
        primitive.positions.push(...positions[v]);
        if (n >= 0) {
          primitive.normals.push(...normals[n]);
          entry.normals++;
        }
        if (t >= 0) {
          primitive.uvs.push(uvs[t][0], 1 - uvs[t][1]);
          entry.uvs++;
        }
        if (hasColors) primitive.colors.push(...colors[v]!, 1);
        ids.set(key, vertex);
      }
      primitive.indices.push(vertex);
    });
    for (const m of [...byMaterial.keys()].sort((a, b) => a - b)) {
      const { primitive, uvs: withUvs, normals: withNormals } = byMaterial.get(m)!;
      // An attribute some corners lack is one the primitive lacks
      const vertices = primitive.positions.length / 3;
      if (withNormals !== vertices) primitive.normals = [];
      if (withUvs !== vertices) primitive.uvs = [];
      primitives.push(primitive);
    }
  }
  return { primitives, materials };
}

// ---------------------------------------------------------------------------------------------
// Vertices

// What a vertex format's attribute takes from models (by its name, as natively): a primitive's
// member, of `size` numbers a vertex, and the number extending it to more (positions' w)
export interface ModelSource {
  readonly member: 'positions' | 'normals' | 'uvs' | 'colors' | 'tangents';
  readonly size: number;
  readonly fill: number;
}

export interface VertexFormatAttribute {
  readonly name: string;
  readonly offset: number;
  readonly format: GPUVertexFormat;
  // The attribute's type's components (its format may have more, which are 0)
  readonly components: number;
  readonly model: ModelSource | null;
  // Its #:default, for vertices without it (as many numbers as components)
  readonly default: readonly number[] | null;
}

// A vertex format as the generated code describes it: how vertex buffers lay out its attributes
export interface VertexFormatDesc {
  readonly name: string;
  readonly stride: number;
  readonly attributes: readonly VertexFormatAttribute[];
}

const word = new Float32Array(1);
const wordBits = new Uint32Array(word.buffer);

// A float's binary16 bits, rounded to the nearest (ties to even), as glm::packHalf
export function toHalf(x: number): number {
  word[0] = x;
  const bits = wordBits[0];
  const sign = (bits >>> 16) & 0x8000;
  const exponent = ((bits >>> 23) & 0xff) - 127 + 15;
  let mantissa = bits & 0x7fffff;
  if (((bits >>> 23) & 0xff) === 0xff) return sign | 0x7c00 | (mantissa ? 0x200 : 0);
  if (exponent >= 0x1f) return sign | 0x7c00;
  // Rounding may carry into the exponent, up to infinity, as it should
  const rounded = (h: number, rest: number, half: number) => (rest > half || (rest === half && (h & 1)) ? h + 1 : h);
  if (exponent <= 0) {
    if (exponent < -10) return sign;
    mantissa |= 0x800000;
    const shift = 14 - exponent;
    return sign | rounded(mantissa >>> shift, mantissa & ((1 << shift) - 1), 1 << (shift - 1));
  }
  return sign | rounded((exponent << 10) | (mantissa >>> 13), mantissa & 0x1fff, 0x1000);
}

// Half away from zero, as std::round
const round = (x: number) => Math.sign(x) * Math.round(Math.abs(x));
const clamp = (x: number, low: number, high: number) => Math.min(Math.max(x, low), high);

function componentWriter(format: GPUVertexFormat): [number, (view: DataView, at: number, x: number) => void] {
  const base = format.replace(/x[0-9]$/, '');
  switch (base) {
    case 'float32': return [4, (v, at, x) => v.setFloat32(at, x, true)];
    case 'uint32': return [4, (v, at, x) => v.setUint32(at, x, true)];
    case 'sint32': return [4, (v, at, x) => v.setInt32(at, x, true)];
    case 'float16': return [2, (v, at, x) => v.setUint16(at, toHalf(x), true)];
    case 'unorm16': return [2, (v, at, x) => v.setUint16(at, round(clamp(x, 0, 1) * 65535), true)];
    case 'snorm16': return [2, (v, at, x) => v.setInt16(at, round(clamp(x, -1, 1) * 32767), true)];
    case 'uint16': return [2, (v, at, x) => v.setUint16(at, x, true)];
    case 'sint16': return [2, (v, at, x) => v.setInt16(at, x, true)];
    case 'unorm8': return [1, (v, at, x) => v.setUint8(at, round(clamp(x, 0, 1) * 255))];
    case 'snorm8': return [1, (v, at, x) => v.setInt8(at, round(clamp(x, -1, 1) * 127))];
    case 'uint8': return [1, (v, at, x) => v.setUint8(at, x)];
    case 'sint8': return [1, (v, at, x) => v.setInt8(at, x)];
  }
  throw new Error(`unknown vertex format ${format}`);
}

// count vertices laid out as format's, attribute a of vertex i being valueOf(a, i)
function encode(format: VertexFormatDesc, count: number, valueOf: (a: VertexFormatAttribute, i: number) => readonly number[]): Uint8Array {
  const bytes = new Uint8Array(format.stride * count);
  const view = new DataView(bytes.buffer);
  for (const a of format.attributes) {
    const [size, write] = componentWriter(a.format);
    for (let i = 0; i < count; i++) {
      const value = valueOf(a, i);
      for (let c = 0; c < a.components; c++) write(view, i * format.stride + a.offset + c * size, value[c]);
    }
  }
  return bytes;
}

// Vertices (objects of attribute names to numbers or vectors, which may leave out attributes with
// defaults) laid out as format's, e.g. for createMesh
export function encodeVertices(format: VertexFormatDesc, vertices: readonly Readonly<Record<string, number | readonly number[]>>[]): Uint8Array {
  return encode(format, vertices.length, (a, i) => {
    const value = vertices[i][a.name] ?? a.default;
    if (value === undefined || value === null) throw new Error(`vertex ${i} has no ${a.name} (a ${format.name} attribute without a default)`);
    return typeof value === 'number' ? [value] : value;
  });
}

// A model primitive's vertices as format's, each attribute from what models give by its name, or
// its default (fromModel natively)
function primitiveVertices(format: VertexFormatDesc, primitive: ModelPrimitive): Uint8Array {
  for (const a of format.attributes) {
    if (a.model && !a.default && primitive[a.model.member].length === 0) {
      throw new Error(`the model has no ${a.model.member}, which ${format.name}'s ${a.name} reads (or give it a #:default)`);
    }
  }
  return encode(format, primitive.positions.length / 3, (a, i) => {
    const source = a.model;
    const values = source ? primitive[source.member] : [];
    if (!source || values.length === 0) return a.default!;
    const value = values.slice(i * source.size, i * source.size + Math.min(source.size, a.components));
    while (value.length < a.components) value.push(source.fill);
    return value;
  });
}

// ---------------------------------------------------------------------------------------------
// Texture arrays

const blitShader = `
@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var pick: sampler;
struct Out {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}
@vertex fn vertexMain(@builtin(vertex_index) i: u32) -> Out {
    let uv = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    return Out(vec4f(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0), uv);
}
@fragment fn fragmentMain(input: Out) -> @location(0) vec4f {
    return textureSampleLevel(source, pick, input.uv, 0.0);
}`;

// Textures (null for white) as the layers of a texture array (what an array binding of textures is
// on the web) of format, the size of the largest: smaller ones are resized, filtered bilinearly.
// They need TEXTURE_BINDING and COPY_SRC usage; the caller destroys them.
export function textureArray(device: GPUDevice, label: string, format: GPUTextureFormat, layers: readonly (GPUTexture | null)[]): GPUTexture {
  if (layers.length > device.limits.maxTextureArrayLayers) {
    throw new Error(`${label} has ${layers.length} images, but this WebGPU device's texture arrays have at most ${device.limits.maxTextureArrayLayers}`);
  }
  let white: GPUTexture | null = null;
  const sources = layers.map((t) => {
    if (t) return t;
    if (!white) {
      white = device.createTexture({ label: `${label} white`, size: [1, 1], format, usage: 0x04 | 0x01 | 0x02 });
      device.queue.writeTexture({ texture: white }, new Uint8Array([255, 255, 255, 255]), { bytesPerRow: 4 }, [1, 1]);
    }
    return white;
  });
  const width = Math.max(1, ...sources.map((t) => t.width));
  const height = Math.max(1, ...sources.map((t) => t.height));
  // TEXTURE_BINDING | COPY_SRC | COPY_DST | RENDER_ATTACHMENT
  const array = device.createTexture({ label, size: [width, height, Math.max(1, layers.length)], format, usage: 0x04 | 0x01 | 0x02 | 0x10 });
  const encoder = device.createCommandEncoder({ label });
  let pipeline: GPURenderPipeline | null = null;
  let sampler: GPUSampler | null = null;
  sources.forEach((source, layer) => {
    if (source.width === width && source.height === height) {
      encoder.copyTextureToTexture({ texture: source }, { texture: array, origin: [0, 0, layer] }, [width, height]);
      return;
    }
    if (!pipeline) {
      const module = device.createShaderModule({ label: `${label} resizing`, code: blitShader });
      pipeline = device.createRenderPipeline({
        label: `${label} resizing`,
        layout: 'auto',
        vertex: { module, entryPoint: 'vertexMain' },
        fragment: { module, entryPoint: 'fragmentMain', targets: [{ format }] },
      });
      sampler = device.createSampler({ magFilter: 'linear', minFilter: 'linear' });
    }
    const pass = encoder.beginRenderPass({
      label: `${label} layer ${layer}`,
      colorAttachments: [{ view: array.createView({ dimension: '2d', baseArrayLayer: layer, arrayLayerCount: 1 }), loadOp: 'clear', storeOp: 'store', clearValue: [0, 0, 0, 0] }],
    });
    pass.setPipeline(pipeline);
    pass.setBindGroup(0, device.createBindGroup({
      layout: pipeline.getBindGroupLayout(0),
      entries: [{ binding: 0, resource: source.createView() }, { binding: 1, resource: sampler! }],
    }));
    pass.draw(3);
    pass.end();
  });
  device.queue.submit([encoder.finish()]);
  (white as GPUTexture | null)?.destroy();
  return array;
}

// ---------------------------------------------------------------------------------------------
// Models on the device

// A model's draws, as the commands of (import indirect)'s DrawIndexedCommand
export interface ModelDraw {
  indexCount: number;
  instanceCount: number;
  firstIndex: number;
  vertexOffset: number;
  firstInstance: number;
}

// A model on the device, as resources/Model natively: every primitive's vertices and indices in one
// mesh, a draw of each (its range of the mesh, one instance, and its material as firstInstance) with
// a bounding sphere, and per material, its base colour factor and texture (a texture array's layer,
// white without one)
export interface Model {
  readonly mesh: Mesh;
  readonly draws: ModelDraw[];
  readonly bounds: number[][];             // Per draw: a bounding sphere, centre (xyz) and radius (w)
  readonly baseColorFactors: number[][];   // Per material
  readonly baseColors: GPUTexture;         // rgba8unorm-srgb, a layer per material
}

// A sphere around positions: their bounding box's centre, and the distance to the farthest
function boundingSphere(positions: readonly number[]): number[] {
  if (positions.length === 0) return [0, 0, 0, 0];
  const low = positions.slice(0, 3);
  const high = positions.slice(0, 3);
  for (let i = 0; i < positions.length; i += 3) {
    for (let c = 0; c < 3; c++) {
      low[c] = Math.min(low[c], positions[i + c]);
      high[c] = Math.max(high[c], positions[i + c]);
    }
  }
  // In single precision, as natively
  const centre = [0, 1, 2].map((c) => Math.fround((low[c] + high[c]) * 0.5));
  let radius = 0;
  for (let i = 0; i < positions.length; i += 3) {
    radius = Math.max(radius, Math.fround(Math.hypot(positions[i] - centre[0], positions[i + 1] - centre[1], positions[i + 2] - centre[2])));
  }
  return [...centre, radius];
}

// Uploads a model, its vertices as format's. Indices are 16-bit when every primitive's vertices
// allow. One image several materials use is loaded once.
export async function uploadModel(context: Context, label: string, data: ModelData, format: VertexFormatDesc): Promise<Model> {
  if (data.primitives.length === 0) throw new Error('uploadModel was given a model without triangles');
  const device = context.device;
  const draws: ModelDraw[] = [];
  const bounds: number[][] = [];
  const vertexBytes: Uint8Array[] = [];
  const indices: number[] = [];
  let vertexCount = 0;
  let small = true;
  for (const primitive of data.primitives) {
    const vertices = primitive.positions.length / 3;
    small = small && vertices <= 65536;
    draws.push({ indexCount: primitive.indices.length, instanceCount: 1, firstIndex: indices.length, vertexOffset: vertexCount, firstInstance: primitive.material });
    bounds.push(boundingSphere(primitive.positions));
    vertexBytes.push(primitiveVertices(format, primitive));
    for (const i of primitive.indices) indices.push(i);
    vertexCount += vertices;
  }
  const vertices = new Uint8Array(vertexBytes.reduce((n, b) => n + b.length, 0));
  vertexBytes.reduce((at, b) => (vertices.set(b, at), at + b.length), 0);

  // Each image once, then the materials' layers
  const textures = new Map<string, Promise<GPUTexture>>();
  const layers = await Promise.all(data.materials.map((material) => {
    const image = material.baseColorTexture;
    if (!image) return null;
    const key = image.path ?? image.description;
    let texture = textures.get(key);
    if (!texture) {
      texture = (async () => {
        try {
          const decoded = await decodeImage(image.encoded ?? await context.loadAsset(image.path!));
          // TEXTURE_BINDING | COPY_SRC | COPY_DST | RENDER_ATTACHMENT (for bitmaps)
          const t = device.createTexture({ label: image.description, size: [decoded.width, decoded.height], format: 'rgba8unorm-srgb', usage: 0x04 | 0x01 | 0x02 | 0x10 });
          writeImage(device, t, decoded);
          return t;
        } catch (error) {
          throw new Error(`loading the base colour texture of material '${material.name}': ${image.path ? `${image.path}: ` : ''}${(error as Error).message}`);
        }
      })();
      textures.set(key, texture);
    }
    return texture;
  }));
  let baseColors: GPUTexture;
  try {
    baseColors = textureArray(device, `${label} base colours`, 'rgba8unorm-srgb', layers);
  } finally {
    for (const t of new Set(layers)) t?.destroy();
  }
  return {
    mesh: createMesh(device, vertices, small ? new Uint16Array(indices) : new Uint32Array(indices), 0, label),
    draws,
    bounds,
    baseColorFactors: data.materials.map((m) => [...m.baseColorFactor]),
    baseColors,
  };
}

// A graph's model (model): loaded and uploaded, its vertices as format's; the graph destroys it.
// what: e.g. "Scene's model ship", for errors.
export async function loadModel(context: Context, graph: RenderGraph, label: string, path: string, format: VertexFormatDesc, what: string): Promise<Model> {
  let data: ModelData;
  try {
    data = await loadModelData(context, path);
  } catch (error) {
    throw new Error(`loading ${what}: ${(error as Error).message}`);
  }
  let model: Model;
  try {
    model = await uploadModel(context, label, data, format);
  } catch (error) {
    throw new Error(`loading ${what} ('${path}'): ${(error as Error).message}`);
  }
  graph.own(model.mesh.vertices);
  graph.own(model.mesh.indices);
  graph.own(model.baseColors);
  return model;
}
