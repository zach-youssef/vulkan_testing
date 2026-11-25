// Compiles WGSL modules with Dawn (WebGPU, Tint, as Chromium does), or with --naga validates them
// with that naga CLI (as Firefox does, though Firefox's naga is its own version), printing their
// errors. Exits with 1 if any has one. Arrays' directive lines (material.ts's expandArrays) are
// expanded as for two elements in two rgba8unorm buckets, at bindings past any the module declares.
// naga 30 can't parse `enable subgroups` (though it validates subgroup builtins and functions
// without it), so --naga drops the extension from enable directives, blanking a directive left
// empty so line numbers stay the module's. It also lacks subgroupElect(), which becomes
// subgroupAll(true), likewise a bool from every active invocation.
//
//     node web/tools/check-wgsl.ts [--naga PATH] FILE.wgsl ...
import { spawnSync } from 'node:child_process';
import { readFileSync } from 'node:fs';

let naga: string | null = null;
const files = process.argv.slice(2);
if (files[0] === '--naga') {
  naga = files[1];
  files.splice(0, 2);
  if (!naga) {
    console.log('usage: check-wgsl.ts [--naga PATH] FILE.wgsl ...');
    process.exit(2);
  }
}

let nextBinding = 100;
const expand = (code: string) =>
  code.replace(/^([ \t]*)\/\/zgal-(array|bucket) \S+ (.*)$/gm, (_, indent: string, kind: string, text: string) => {
    if (kind === 'array') return indent + text.replaceAll('{count}', '2');
    const first = (nextBinding += 2) - 2;
    return [0, 1].map((i) => indent + text.replaceAll('{bucket}', String(i)).replaceAll('{binding}', String(first + i)).replaceAll('{format}', 'rgba8unorm')).join('\n');
  });

// Each module's errors as printable lines (none if it compiles)
type Check = (file: string, code: string) => Promise<string[]>;

// Dawn's instance, kept alive at module scope (a collected instance takes its devices with it)
let gpu: GPU | null = null;
async function dawn(): Promise<Check> {
  const { create, globals } = await import('webgpu');
  Object.assign(globalThis, globals);
  gpu = create([]);
  const adapter = await gpu.requestAdapter();
  if (!adapter) {
    console.log('No WebGPU adapter');
    process.exit(1);
  }
  const features = (['subgroups'] as GPUFeatureName[]).filter((f) => adapter.features.has(f));
  const device = await adapter.requestDevice({ requiredFeatures: features });
  return async (_file, code) => {
    const lines = code.split('\n');
    const module = device.createShaderModule({ code });
    const errors = (await module.getCompilationInfo()).messages.filter((m) => m.type === 'error');
    return errors.map((e) => `${e.lineNum}:${e.linePos}: ${e.message}\n    ${lines[e.lineNum - 1]}`);
  };
}

// naga reads the expanded module from stdin, its errors naming the file (line numbers are the
// expanded module's), uncoloured (it colours them even when not writing to a terminal)
function nagaCheck(path: string): Check {
  return async (file, code) => {
    const run = spawnSync(path, ['--input-kind', 'wgsl', '--stdin-file-path', file], { input: code, encoding: 'utf8', env: { ...process.env, NO_COLOR: '1' } });
    if (run.error) {
      console.log(`check-wgsl: can't run naga (${path}): ${run.error.message}`);
      process.exit(1);
    }
    return run.status === 0 ? [] : [(run.stdout + run.stderr).trim()];
  };
}

// The module as naga 30 parses it: without the subgroups extension in its enable directives, and
// subgroupElect() (which it lacks) as subgroupAll(true)
const forNaga = (code: string) =>
  code.replaceAll('subgroupElect()', 'subgroupAll(true)').replace(/^([ \t]*)enable ([^;]*);/gm, (_, indent: string, list: string) => {
    const kept = list.split(',').map((e) => e.trim()).filter((e) => e !== 'subgroups');
    return kept.length > 0 ? `${indent}enable ${kept.join(', ')};` : '';
  });

const check = naga ? nagaCheck(naga) : await dawn();
let failed = 0;
for (const file of files) {
  const expanded = expand(readFileSync(file, 'utf8'));
  const code = naga ? forNaga(expanded) : expanded;
  const errors = await check(file, code);
  if (errors.length > 0) {
    failed++;
    console.log(`${file}:`);
    for (const e of errors) console.log(e.replace(/^/gm, '  '));
  }
}
console.log(`${failed} of ${files.length} modules failed${naga ? ' naga' : ''}`);
process.exit(failed > 0 ? 1 : 0);
