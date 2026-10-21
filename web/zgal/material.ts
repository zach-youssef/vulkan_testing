// Materials as the generated code describes them, and the pipelines and bind groups the runtime
// makes from them. WebGPU has no bindless descriptors: a material's bindings are bind group 0
// (binding i is its i'th binding), and its push constants a uniform buffer after them.

import type { Type } from './types.ts';

export type Stage = 'vertex' | 'fragment' | 'compute';
export type BindingKind = 'texture' | 'sampler' | 'storage-image' | 'buffer' | 'buffer-array';
export type Access = 'read' | 'write' | 'read-write';

export interface BindingDesc {
  readonly name: string;
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
// textures and storage images
export interface BoundFormats {
  readonly textures: Readonly<Record<string, GPUTextureFormat>>;
  readonly samplers: Readonly<Record<string, 'filtering' | 'non-filtering'>>;
}

// The bind group layout entries of a material bound to resources of `formats`
export function layoutEntries(device: GPUDevice, m: MaterialDesc, formats: BoundFormats): GPUBindGroupLayoutEntry[] {
  const filterable = device.features.has('float32-filterable');
  const stagesOf = (b: BindingDesc) => {
    const used = b.stages.length > 0 ? b.stages : m.kind === 'compute' ? ['compute' as Stage] : ['vertex' as Stage, 'fragment' as Stage];
    return used.reduce((flags, s) => flags | stageFlags[s], 0);
  };
  const entries: GPUBindGroupLayoutEntry[] = m.bindings.map((b, i) => {
    const visibility = stagesOf(b);
    switch (b.kind) {
      case 'texture': {
        const format = formats.textures[b.name];
        const sampleType: GPUTextureSampleType = b.depth
          ? 'depth'
          : format && (isDepthFormat(format) || (!filterable && unfilterable.has(format)))
            ? 'unfilterable-float'
            : 'float';
        return { binding: i, visibility, texture: { sampleType } };
      }
      case 'sampler':
        return { binding: i, visibility, sampler: { type: b.depth ? 'comparison' : (formats.samplers[b.name] ?? 'filtering') } };
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
  return entries;
}

// The storage formats a material's shaders declare
export function storageFormats(m: MaterialDesc, formats: BoundFormats): StorageFormats {
  const result: Record<string, GPUTextureFormat> = {};
  for (const b of m.bindings) {
    if (b.kind === 'storage-image') result[b.name] = formats.textures[b.name] ?? 'rgba8unorm';
  }
  return result;
}

// Writes push constants (field name -> value) into bytes laid out as m's ZgalPush
export function writePush(m: MaterialDesc, values: Readonly<Record<string, unknown>>, into: DataView): void {
  if (!m.push) return;
  for (const [name, field] of Object.entries(m.push.fields)) {
    if (name in values) field.type.write(into, field.offset, values[name]);
  }
}
