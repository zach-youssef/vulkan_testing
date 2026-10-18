// Apps and functions compiled to TypeScript: functions compute what the GPU (and C++) would, and
// apps handle events, run effects, read readbacks' copies, call externs and choose graphs as C++
// apps do
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import * as zgal from '../zgal/index.ts';
import { createNodeContext } from '../zgal/node.ts';
import { Evaluating, Probe, collatz, evaluate, exact, triangle } from '../apps.zgal.ts';

let context: zgal.Context;
const printed: string[] = [];

before(async () => {
  context = await createNodeContext();
  zgal.output.line = (text) => printed.push(text);
});

after(() => context.destroy());

test('functions compute on the CPU what they do on the GPU', async () => {
  const graph = new zgal.RenderGraph(context);
  const count = 200;
  const ids = await Evaluating.build(context, graph, { count });
  await graph.execute();
  const gpu = await ids.readEvaluatedContents(graph);
  for (let i = 0; i < count; i++) {
    const cpu = evaluate(i);
    // Integer arithmetic: exact
    assert.equal(gpu[i][3], cpu[3], `element ${i}`);
    for (let c = 0; c < 3; c++) assert.ok(Math.abs(gpu[i][c] - cpu[c]) <= 1e-4 * Math.max(1, Math.abs(cpu[c])), `element ${i}[${c}]: ${gpu[i][c]} vs ${cpu[c]}`);
  }
  graph.destroy();
});

test('values have C++\'s semantics: 32-bit floats, wrapping and truncating integers, vectors and matrices', () => {
  const f = Math.fround;
  // Floats given to generated code are 32-bit, as C++'s float parameters would make them
  assert.deepEqual(exact(f(0.1), f(0.2), 65537, 0), {
    sum: f(f(0.1) + f(0.2)),
    product: 131073, // 65537 squared, wrapped
    quotient: -3,
    remains: -1,
    under: 4294967295,
    shifted: 2147483648,
    narrowed: 44,
    signedNarrowed: -120,
    scaled: [f(0.1), f(2 * f(0.1)), f(3 * f(0.1))],
    transformed: [4, 6],
    swapped: [f(0.2), f(0.1)],
    truncated: [-1, 2],
    // GLM negates the zeros too
    inverted: [[0.5, -0], [-0, 0.25]],
  });
  assert.equal(triangle(10), 55);
  assert.equal(collatz(27), 111);
});

async function probe() {
  const graphs = { waiting: new zgal.RenderGraph(context), shown: new zgal.RenderGraph(context) };
  const notes: number[] = [];
  const app = await Probe.create(context, graphs, {
    base: 10,
    label: 0.5,
    timestep: zgal.Timestep.fixed(0.25),
    note: (x) => {
      notes.push(x);
      return x * 2;
    },
  });
  const frame = async () => {
    app.beginFrame();
    await app.renderGraph().execute();
  };
  const destroy = () => {
    graphs.waiting.destroy();
    graphs.shown.destroy();
  };
  return { app, graphs, notes, frame, destroy };
}

test('handlers compute the next state from events, the clock and externs', async () => {
  const { app, notes, frame, destroy } = await probe();
  assert.equal(app.state.started, false);
  app.handle({ kind: 'key-pressed', key: 'Space' });
  app.handle({ kind: 'key-pressed', key: 'A' });
  app.handle({ kind: 'key-pressed', key: 'B' }); // No handler
  assert.equal(app.state.presses, 2);
  app.handle({ kind: 'mouse-pressed', button: 'left', cursor: [0.5, -0.5] });
  app.handle({ kind: 'mouse-pressed', button: 'right', cursor: [1, 1] }); // No handler
  app.handle({ kind: 'cursor-moved', cursor: [0.25, 0.75] });
  app.handle({ kind: 'scrolled', offset: [0, 2] });
  app.handle({ kind: 'scrolled', offset: [0, -0.5] });
  assert.deepEqual(app.state.held, [0.5, -0.5]);
  assert.deepEqual(app.state.cursor, [0.25, 0.75]);
  assert.equal(app.state.scroll, 1.5);
  await frame();
  await frame();
  assert.equal(app.state.time, 0.5);
  // The extern saw each frame's number, and the state holds what it gave
  assert.deepEqual(notes, [0, 1]);
  assert.equal(app.state.noted, 2);
  // print! writes C++'s way: floats as %g, bools as words
  assert.deepEqual(printed.slice(-2), ['pressed 1 at 0.5 false', 'pressed 2 at 0.5 false']);
  destroy();
});

test('effects: quit!, resize!, set-frames-in-flight! and run-once!', async () => {
  const { app, frame, destroy } = await probe();
  assert.equal(app.quitRequested(), false);
  app.handle({ kind: 'key-pressed', key: 'Escape' });
  assert.equal(app.quitRequested(), true);
  app.handle({ kind: 'key-pressed', key: 'R' });
  assert.deepEqual(app.takeResize(), [32, 16]);
  assert.equal(app.takeResize(), null);
  assert.equal(context.framesInFlight, 3);
  context.setFramesInFlight(2);

  app.handle({ kind: 'key-released', key: 'S' });
  const shown = app.shownIds;
  await frame();
  // The once node ran in the first frame of its graph
  assert.equal(app.renderGraph().isEnabled(shown.nodes.onceFilling), false);
  app.handle({ kind: 'key-pressed', key: 'O' });
  assert.equal(app.renderGraph().isPendingOnce(shown.nodes.onceFilling), true);
  await frame();
  assert.equal(app.renderGraph().isPendingOnce(shown.nodes.onceFilling), false);
  destroy();
});

test('phases: the first graph whose condition holds runs, with inputs and enables from the state', async () => {
  const { app, graphs, frame, destroy } = await probe();
  await frame();
  assert.equal(app.renderGraph(), graphs.waiting);
  app.handle({ kind: 'key-released', key: 'S' });
  app.handle({ kind: 'key-pressed', key: 'Space' });
  await frame();
  assert.equal(app.renderGraph(), graphs.shown);
  // base + presses, then the next element
  assert.deepEqual(await app.shownIds.readOutContents(graphs.shown), [11, 12]);
  assert.deepEqual(await app.shownIds.readExtraContents(graphs.shown), [101]);
  // Toggling extraFilling off leaves its buffer as the last frame wrote it
  app.handle({ kind: 'key-pressed', key: 'E' });
  app.handle({ kind: 'key-pressed', key: 'Space' });
  await frame();
  assert.equal(graphs.shown.isEnabled(app.shownIds.nodes.extraFilling), false);
  assert.deepEqual(await app.shownIds.readOutContents(graphs.shown), [12, 13]);
  assert.deepEqual(await app.shownIds.readExtraContents(graphs.shown), [101]);
  destroy();
});

test('read events give the state readbacks\' finished copies', async () => {
  const { app, graphs, frame, destroy } = await probe();
  app.handle({ kind: 'key-released', key: 'S' });
  await frame();
  await graphs.shown.finish();
  // The copy finished after the frame: the next frame's handler reads it
  assert.deepEqual(app.state.seen, []);
  await frame();
  assert.deepEqual(app.state.seen, [10, 11]);
  destroy();
});

test('WebGPU reports no validation errors', async () => {
  await context.device.queue.onSubmittedWorkDone();
  assert.deepEqual(context.errors, []);
});
