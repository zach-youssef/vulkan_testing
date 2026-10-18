// Running a generated main under Node, headless on Dawn: its command line, frames, and captures
// written to PNG files. Exit statuses as the native mains': 0, 1 for an error or WebGPU errors, 2 for
// a bad command line.

import { mkdir, writeFile } from 'node:fs/promises';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { RenderGraph } from './graph.ts';
import { Launch, capturePixels, parseCommandLine, type Program, type RunnableApp } from './main.ts';
import { createNodeContext } from './node.ts';
import { encodePng } from './png.ts';

export async function runNode<A extends RunnableApp, I>(program: Program<A, I>, url: string): Promise<void> {
  const inputs = program.defaults();
  const parsed = parseCommandLine(program, process.argv.slice(2), inputs, 'node');
  if (!(parsed instanceof Launch)) {
    (parsed.status === 0 ? process.stdout : process.stderr).write(parsed.text);
    process.exitCode = parsed.status;
    return;
  }
  const launch = parsed;
  // Assets are found in assets/ beside the program, as the native programs find theirs
  const context = await createNodeContext([join(dirname(fileURLToPath(url)), 'assets')]);
  const graphs: RenderGraph[] = [];
  try {
    const app = await program.create(context, launch, inputs, () => {
      const graph = new RenderGraph(context);
      graph.setSurfaceExtent(launch.size);
      graphs.push(graph);
      return graph;
    });
    const frames = launch.frames ?? 1;
    for (let frame = 0; frame < frames && !app.quitRequested(); frame++) {
      app.beginFrame();
      const size = app.takeResize();
      if (size) app.setSurfaceExtent(size);
      await app.renderGraph().execute();
      if (launch.captureFrames.includes(frame + 1)) {
        await writeCapture(program, app, join(launch.capture!, `${frame + 1}.png`));
      }
    }
    for (const graph of graphs) await graph.finish();
    if (launch.capture !== null && launch.captureFrames.length === 0) {
      await writeCapture(program, app, launch.capture);
      if (program.captureOption) console.log(`Wrote ${launch.capture}`);
    }
  } catch (error) {
    process.stderr.write(`${program.name}: ${error instanceof Error ? error.message : String(error)}\n`);
    process.exitCode = 1;
  } finally {
    for (const graph of graphs) graph.destroy();
  }
  // WebGPU's validation errors fail the run, as validation messages fail native runs with --validation
  await context.device.queue.onSubmittedWorkDone();
  for (const error of context.errors) process.stderr.write(`[WebGPU] ${error}\n`);
  if (context.errors.length > 0) {
    process.stderr.write(`${program.name}: ${context.errors.length} WebGPU error(s)\n`);
    process.exitCode = 1;
  }
  context.destroy();
}

async function writeCapture<A extends RunnableApp, I>(program: Program<A, I>, app: A, path: string): Promise<void> {
  if (!program.capture) throw new Error('the program captures nothing');
  const { graph, node } = program.capture(app);
  const { pixels, size } = await capturePixels(graph, node);
  await mkdir(dirname(path), { recursive: true });
  await writeFile(path, encodePng(size[0], size[1], pixels));
}
