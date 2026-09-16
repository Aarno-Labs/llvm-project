// RUN: env CLANG_REFOLD_TEST_ONLY_SEMANTIC_REALIZATION_COST_BUDGET=0 %clang-refold-tester macro_expansion_hunk_edge_retracts_outer_left_edge_into_expansion
// A hunk that begins in plain translation-unit tokens and ends inside a macro
// expansion must retract its outer edge onto the expansion boundary when the
// edge inside the expansion cannot move.
//
// Rewriting the argument `ob->index[j]` to `(ot->storage)[ob_index].index[j]`
// adds one `(` to the run that `IN_USE`'s `((x) >= 0)` body shares with the
// `if` paren.  Every placement of the new paren is equally optimal, so the
// certifier forces only `if` and `->`, and the core-forced hunk `( ( ( ob`
// takes the `if` paren along with the start of the expansion.  Neither owner
// can realize that: the callsite does not hold the `if` paren, and the
// translation unit cannot realize part of an expansion.  Retracting the edge
// inside the expansion fails on `ob` against `ot`, and widening stops at the
// neighbouring insertion inside the argument.  The `if` paren is the same on
// both sides, though, so handing it back to the untouched region leaves the
// hunk inside the expansion, and the callsite replay keeps `IN_USE`.
//
// The realization budget is zeroed so the alignment resolver cannot commit a
// narrower map and the core-forced plan has to stand on its own, as it does
// when independent ambiguity elsewhere in the same window leaves the resolver
// with no commit rule.  Regression for the tenjin stb_ds `arr_del` refold,
// which failed with `no admissible refold` on `NoOwnerClosedCover`.
#define IN_USE(x) ((x) >= 0)

struct slot {
  int index[4];
};
struct table {
  struct slot *storage;
};

int probe(struct table *ot, struct slot *ob, int ob_index, int j) {
  if (IN_USE(ob->index[j])) {
    return 1;
  }
  return 0;
}
