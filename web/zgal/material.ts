// Materials as the generated code describes them, and the pipelines and bind groups the runtime
// makes from them. WebGPU has no bindless descriptors: a material's bindings are bind group 0
// (binding i is the i'th of its description's, in the material's order), and its push constants a
// uniform buffer after them.

import type { Type } from './types.ts';

export type Stage = 'vertex' | 'fragment' | 'compute';
// A texture-array is an array binding of textures (WebGPU has no arrays of bindings), bound as a
// TextureArray: its binding is a uniform table of its elements, and its buckets (texture_2d_arrays,
// one per distinct size and format among the elements) bind after the push constants (see
// expandArrays). WebGPU's samplers each do one thing, so a ZGAL sampler
// is up to three bindings of one resource (a Sampler), the variants its stages use: sampler, as
// the graph describes it; depth-sampler (zgalDepth_<name>), a non-filtering one sampling depth
// textures, which WebGPU refuses filtering samplers on; and comparison-sampler
// (zgalCompare_<name>), comparing depths
export type BindingKind = 'texture' | 'texture-array' | 'sampler' | 'depth-sampler' | 'comparison-sampler' | 'storage-image' | 'buffer' | 'buffer-array';
export type Access = 'read' | 'write' | 'read-write';

export interface BindingDesc {
  // Its name in the material's WGSL and its description
  readonly name: string;
  // The ZGAL binding it's bound from (the name a graph gives a resource for)
  readonly resource: string;
  readonly kind: BindingKind;
  readonly access: Access | null;
  // The stages using it
  readonly stages: readonly Stage[];
  // A depth texture (compared with depths) or a comparison sampler
  readonly depth: boolean;
}

export interface PushField {
  readonly offset: number;
  readonly type: Type<any>;
}

export interface PushDesc {
  // The uniform buffer's binding and size (a multiple of 16)
  readonly binding: number;
  readonly size: number;
  readonly fields: Readonly<Record<string, PushField>>;
}

// The texel formats of a material's storage images, by binding name, which WGSL declares
export type StorageFormats = Readonly<Record<string, GPUTextureFormat>>;
export type ShaderSource = (formats: StorageFormats) => string;

export interface VertexAttributeDesc {
  readonly location: number;
  readonly offset: number;
  readonly format: GPUVertexFormat;
}

export interface VertexInputDesc {
  readonly stride: number;
  readonly attributes: readonly VertexAttributeDesc[];
}

interface MaterialDescBase {
  readonly name: string;
  // Translated from GLSL bodies (glslc and naga): one binding per ZGAL binding, its samplers
  // WebGPU's own (a comparison-sampler compares in the sampler, and there's no border colour), and
  // no pipeline-overridable constants. A ZGAL body's module does what WebGPU's samplers can't by
  // hand, from the constants the runtime sets (MaterialInstance's constants)
  readonly glsl: boolean;
  readonly bindings: readonly BindingDesc[];
  readonly push: PushDesc | null;
}

export interface ComputeMaterialDesc extends MaterialDescBase {
  readonly kind: 'compute';
  readonly workgroup: readonly [number, number, number];
  readonly compute: ShaderSource;
  // For a library kernel using subgroup operations, the same kernel without them (shared memory
  // instead), for devices without the subgroups feature
  readonly withoutSubgroups?: ShaderSource;
}

export interface GraphicsMaterialDesc extends MaterialDescBase {
  readonly kind: 'graphics';
  readonly vertexInput: VertexInputDesc | null;
  readonly colorOutputs: number;
  readonly vertex: ShaderSource;
  readonly fragment: ShaderSource;
}

export type MaterialDesc = ComputeMaterialDesc | GraphicsMaterialDesc;

// Workgroups covering invocations (a dispatch's size) in each dimension
export function workgroupsFor(m: ComputeMaterialDesc, invocations: readonly number[]): [number, number, number] {
  return [0, 1, 2].map((i) => Math.ceil((invocations[i] ?? 1) / m.workgroup[i])) as [number, number, number];
}

const stageFlags: Record<Stage, number> = {
  vertex: 0x1, // GPUShaderStage.VERTEX
  fragment: 0x2, // GPUShaderStage.FRAGMENT
  compute: 0x4, // GPUShaderStage.COMPUTE
};

// Formats storage images can be read and written in on any WebGPU device (others need the
// texture-formats-tier2 feature)
const readWriteCore = new Set<GPUTextureFormat>(['r32float', 'r32uint', 'r32sint']);

// Formats sampling can't filter without the float32-filterable feature
const unfilterable = new Set<GPUTextureFormat>(['r32float', 'rg32float', 'rgba32float', 'r32uint', 'r32sint']);

export function isDepthFormat(f: GPUTextureFormat): boolean {
  return f.startsWith('depth');
}

// What a material's bindings are bound to, as far as its pipeline cares: the formats of its
// textures and storage images, the shape of its arrays of textures (their number of elements and
// their buckets' formats), and whether its samplers (the plain ones: depth and comparison samplers
// are what they are) filter
export interface BoundFormats {
  readonly textures: Readonly<Record<string, GPUTextureFormat>>;
  readonly arrays: Readonly<Record<string, ArrayShape>>;
  readonly samplers: Readonly<Record<string, 'filtering' | 'non-filtering'>>;
}

export interface ArrayShape {
  readonly count: number;
  readonly buckets: readonly GPUTextureFormat[];
}

// The binding index of each array binding's first bucket: after the push constants' binding (the
// bindings' number, even without push constants), in the bindings' order
export function firstBuckets(m: MaterialDesc, formats: BoundFormats): Record<string, number> {
  const result: Record<string, number> = {};
  let next = m.bindings.length + 1;
  for (const b of m.bindings) {
    if (b.kind !== 'texture-array') continue;
    result[b.name] = next;
    next += formats.arrays[b.name]?.buckets.length ?? 0;
  }
  return result;
}

// A module (as its ShaderSource gives it) with its arrays of textures' directive lines expanded
// (wgsl.rkt's "Arrays of textures"): `//zgal-array <name> <text>` becomes text, its {count} the
// array's number of elements, and `//zgal-bucket <name> <text>` becomes text for each bucket, its
// {bucket} the bucket's number and {binding} its binding index
export function expandArrays(code: string, m: MaterialDesc, formats: BoundFormats): string {
  if (!code.includes('//zgal-')) return code;
  const first = firstBuckets(m, formats);
  return code.replace(/^([ \t]*)\/\/zgal-(array|bucket) (\S+) (.*)$/gm, (_, indent: string, kind: string, name: string, text: string) => {
    const shape = formats.arrays[name];
    if (!shape) throw new Error(`material ${m.name}: array ${name} isn't bound to a TextureArray`);
    if (kind === 'array') return indent + text.replaceAll('{count}', String(shape.count));
    return shape.buckets
      .map((_, i) => indent + text.replaceAll('{bucket}', String(i)).replaceAll('{binding}', String(first[name] + i)))
      .join('\n');
  });
}

// The module a material's shader source gives for what it's bound to
export function shaderCode(m: MaterialDesc, source: ShaderSource, formats: BoundFormats): string {
  return expandArrays(source(storageFormats(m, formats)), m, formats);
}

// The bind group layout entries of a material bound to resources of `formats`
export function layoutEntries(device: GPUDevice, m: MaterialDesc, formats: BoundFormats): GPUBindGroupLayoutEntry[] {
  const filterable = device.features.has('float32-filterable');
  const stagesOf = (b: BindingDesc) => {
    const used = b.stages.length > 0 ? b.stages : m.kind === 'compute' ? ['compute' as Stage] : ['vertex' as Stage, 'fragment' as Stage];
    return used.reduce((flags, s) => flags | stageFlags[s], 0);
  };
  const sampleType = (format: GPUTextureFormat | undefined, depth: boolean): GPUTextureSampleType =>
    depth ? 'depth' : format && (isDepthFormat(format) || (!filterable && unfilterable.has(format))) ? 'unfilterable-float' : 'float';
  const buckets: GPUBindGroupLayoutEntry[] = [];
  const first = firstBuckets(m, formats);
  const entries: GPUBindGroupLayoutEntry[] = m.bindings.map((b, i) => {
    const visibility = stagesOf(b);
    switch (b.kind) {
      case 'texture':
        return { binding: i, visibility, texture: { sampleType: sampleType(formats.textures[b.name], b.depth) } };
      case 'texture-array': {
        (formats.arrays[b.name]?.buckets ?? []).forEach((format, bucket) => {
          buckets.push({ binding: first[b.name] + bucket, visibility, texture: { sampleType: sampleType(format, false), viewDimension: '2d-array' } });
        });
        // The table of its elements
        return { binding: i, visibility, buffer: { type: 'uniform' } };
      }
      case 'sampler':
        return { binding: i, visibility, sampler: { type: formats.samplers[b.name] ?? 'filtering' } };
      case 'depth-sampler':
        return { binding: i, visibility, sampler: { type: 'non-filtering' } };
      case 'comparison-sampler':
        return { binding: i, visibility, sampler: { type: 'comparison' } };
      case 'storage-image': {
        const format = formats.textures[b.name];
        if (b.access !== 'read' && b.access !== 'write' && format && !readWriteCore.has(format) && !device.features.has('texture-formats-tier2')) {
          throw new Error(`material ${m.name}: storage image ${b.name} is read-write, which this WebGPU device only allows for r32float, r32uint and r32sint images (not ${format}) without the texture-formats-tier2 feature: bind it read or write`);
        }
        return {
          binding: i,
          visibility,
          storageTexture: {
            access: b.access === 'read' ? 'read-only' : b.access === 'write' ? 'write-only' : 'read-write',
            format,
          },
        };
      }
      case 'buffer':
      case 'buffer-array':
        return { binding: i, visibility, buffer: { type: b.access === 'read' ? 'read-only-storage' : 'storage' } };
    }
  });
  if (m.push) {
    const visibility = m.kind === 'compute' ? stageFlags.compute : stageFlags.vertex | stageFlags.fragment;
    entries.push({ binding: m.push.binding, visibility, buffer: { type: 'uniform' } });
  }
  return [...entries, ...buckets];
}

// The storage formats a material's shaders declare
export function storageFormats(m: MaterialDesc, formats: BoundFormats): StorageFormats {
  const result: Record<string, GPUTextureFormat> = {};
  for (const b of m.bindings) {
    if (b.kind === 'storage-image') result[b.name] = formats.textures[b.name] ?? 'rgba8unorm';
  }
  return result;
}

// Writes push constants (field name -> value) into bytes laid out as m's push description gives
// (its fields' offsets and types: std430's layout, or a uniform buffer's for arrays and structs)
export function writePush(m: MaterialDesc, values: Readonly<Record<string, unknown>>, into: DataView): void {
  if (!m.push) return;
  for (const [name, field] of Object.entries(m.push.fields)) {
    if (name in values) field.type.write(into, field.offset, values[name]);
  }
}
