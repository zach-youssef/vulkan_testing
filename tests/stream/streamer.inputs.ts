// What streamer.zgal's app needs that its main can't give: the opener of its stream-buffer's
// stream, which streams the file through the context's assets
import type * as zgal from './zgal/index.ts';
import type { MainInputs } from './streamer.zgal.ts';

export function mainInputs(_context: zgal.Context): MainInputs {
  return { open: (path, context) => context.loadAssetStream(path) };
}
