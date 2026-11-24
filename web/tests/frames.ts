// Helpers the Node tests share
import type * as zgal from '../zgal/index.ts';

// Puts the context's frames in flight back to count at once (cancelling a change still pending),
// so a test failing part way doesn't leave later ones a different count: for a test's finally
export function restoreFrames(context: zgal.Context, count: number): void {
  context.setFramesInFlight(count);
  if (context.framesInFlight !== count) context.advanceFrame();
}
