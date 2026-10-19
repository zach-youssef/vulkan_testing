// Compiles WGSL modules with Dawn (WebGPU), printing their errors. Exits with 1 if any has one.
//
//     node web/tools/check-wgsl.ts FILE.wgsl ...
import { readFileSync } from 'node:fs';
import { create, globals } from 'webgpu';

Object.assign(globalThis, globals);
// Kept alive (a collected instance takes its devices with it)
const gpu = create([]);
const adapter = await gpu.requestAdapter();
if (!adapter) {
  console.log('No WebGPU adapter');
  process.exit(1);
}
const features = (['subgroups'] as GPUFeatureName[]).filter((f) => adapter.features.has(f));
const device = await adapter.requestDevice({ requiredFeatures: features });
let failed = 0;
for (const file of process.argv.slice(2)) {
  const code = readFileSync(file, 'utf8');
  const lines = code.split('\n');
  const module = device.createShaderModule({ code });
  const errors = (await module.getCompilationInfo()).messages.filter((m) => m.type === 'error');
  if (errors.length > 0) {
    failed++;
    console.log(`${file}:`);
    for (const e of errors) console.log(`  ${e.lineNum}:${e.linePos}: ${e.message}\n    ${lines[e.lineNum - 1]}`);
  }
}
console.log(`${failed} of ${process.argv.length - 2} modules failed`);
process.exit(failed > 0 ? 1 : 0);
