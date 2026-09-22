// RUN: %clang-refold-tester macro_boundary_literal_one_of_three_alignments_admissible
// RUN: FileCheck --input-file=%t/outputs/macro_boundary_literal_one_of_three_alignments_admissible.out %s
//
// The actual of `IN_USE` gains a leading `(`, so B's `(((` has one more `(`
// than A's `((`: the `if`'s own `(` followed by the replacement list's `(`.
// Which B `(` is new is a three-way tie between core-optimal maps:
//
//   1. the third, inside the actual     -> `IN_USE((*p)[j])`
//   2. the second, before the invocation -> TU text before `IN_USE`
//   3. the first, before the `if`'s `(`  -> TU text before that `(`
//
// Maps 2 and 3 keep the replacement list's `(` aligned to B's argument paren,
// so direct TU text would have to supply a replacement-list literal from
// outside the invocation.  Spliced naively they give `if ((IN_USE(*p)[j]))`,
// which applies `>= 0` to `*p` instead of the element.  Both must be refused,
// leaving the per-window semantic resolver exactly one admissible map, which
// it commits.
//
// This is the minimal form of macro_boundary_literal_is_not_a_tu_insertion,
// whose two-paren replacement list yields four maps, two of them accepted.
//
// CHECK: 3 distinct core-optimal map(s) enumerated
// CHECK: candidate map 1 of 3 realized: accepted
// CHECK: rejecting direct-TU insertion at A gap 14: B=[14,15) would supply a replacement-list literal of macro {{[0-9]+}} (IN_USE)
// CHECK: candidate map 2 of 3 realized: requested terminal fallback
// CHECK: rejecting direct-TU insertion at A gap 13: B=[13,14) would supply a replacement-list literal of macro {{[0-9]+}} (IN_USE)
// CHECK: candidate map 3 of 3 realized: requested terminal fallback
// CHECK: candidate census: enumerated=3 realized=3 accepted=1 terminalFallback=2 proofIncomplete=0
// CHECK: committed one realized-source class: 1 of 3 enumerated map(s) share it
#define IN_USE(x) (x >= 0)

int probe(int **p, int j) {
  if (IN_USE((*p)[j]))
    return 1;
  return 0;
}
