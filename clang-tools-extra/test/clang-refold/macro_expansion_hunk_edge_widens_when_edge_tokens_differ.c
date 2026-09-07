// RUN: %clang-refold-tester macro_expansion_hunk_edge_widens_when_edge_tokens_differ
// A hunk edge that falls strictly inside a macro expansion must land on a
// whole-expansion boundary before the edit is planned, and retraction is not
// always the move that gets it there.
//
// `(p != NIL)` aligns against `((p && n >= 0))` with the trailing `0 ) )` as
// its only forced anchors: the two candidate prefix pairings are equally
// optimal so the certifier keeps neither, while dropping any of the trailing
// three would cost a match.  Two of those forced anchors are the `0` and `)`
// of `NIL`'s `((void*)0)` expansion, matched against a `0` and a `)` that this
// edit newly wrote, which leaves the hunk ending two tokens inside the
// expansion.  The token objective scores lexemes, not expansions, so this is
// not ambiguity the certifier can rule out -- it is the optimum.
//
// Retraction cannot repair the edge, because the tokens there are `)` in A and
// `>=` in B: handing them back to the untouched region would not reproduce
// them.  Widening takes the rest of the expansion into the hunk instead, so a
// single replacement consumes the whole callsite -- which is what this edit
// means, `NIL` appearing nowhere in B.  Without it no owner can realize a
// partial cover and the whole translation unit escalates to raw B.
//
// Regression for the tenjin `envy_lib` c_17_refold_preprocessor defect.
#define NIL ((void*)0)

int probe(char *p, int n)
{
  if (p != NIL) {
    return 1;
  }
  return 2;
}
