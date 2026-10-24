// What painter.zgal's app needs that its main can't give, on the web (painter.cpp natively): the
// scale no option sets, the extern function and the graph's data
import type * as zgal from './zgal/index.ts';
import type { MainInputs } from './painter.zgal.ts';

export function mainInputs(_context: zgal.Context): MainInputs {
  return {
    scale: 0.5,
    note: (level) => {
      console.log(`level ${level}`);
      return level;
    },
    marks: [1, 2, 3],
  };
}
