/* A stable sort: webOS 6's Node 8 (V8 6.2) reorders ties in lists of more than 10, where newer Node keeps
 * them in order (the API's, or the order we list them in). Sorts list in place and returns it, as sort() does. */
'use strict';

function sortStable(list, compare) {
  const keyed = list.map((v, i) => ({ v, i }));
  keyed.sort((a, b) => compare(a.v, b.v) || a.i - b.i);
  for (let i = 0; i < keyed.length; i++) list[i] = keyed[i].v;
  return list;
}

module.exports = { sortStable };
