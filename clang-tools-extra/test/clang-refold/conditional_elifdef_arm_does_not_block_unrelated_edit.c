// RUN: %clang-refold-tester conditional_elifdef_arm_does_not_block_unrelated_edit
// Regression: the producer's conditional scanner must record `#elifdef` and
// `#elifndef` arms, or every edit in the translation unit becomes unrefoldable.
//
// The consumer binds a lexical conditional group to its producer record only on
// an exact match: same opening boundary, same `#endif` end, same arm count, and
// each arm's recorded kind and body start equal to the scanned control line.
// The scanner recognized `#if`/`#ifdef`/`#ifndef`/`#elif`/`#else`/`#endif` and
// nothing else, and its keyword matcher requires an identifier boundary, so
// `#elifdef` matched neither `elif` nor anything else and produced no arm at
// all.  The group was then recorded with one arm where the source has two:
//
//   conditional group id=0 arm count mismatch: model=1 source=2
//   producer conditional group id=0 range=[...] has no unique exact lexical
//   binding
//
// Losing the binding costs the whole file, not the group: with no bound group
// the structure index cannot close an owner cover for anything, so an edit that
// shares no owner, line or state with the conditional still refused with
// `obligation=OwnerClosedCover reason=NoOwnerClosedCover stage=classify`.  That
// is what this test checks -- the edit below is a single token in a declaration
// that touches neither group.
//
// Both C23 spellings are covered because they are one rule, and both arms here
// are taken and contribute tokens, so the arm's `selected` flag and `pp_span`
// are exercised rather than just its extent.
#define ELIFDEF_Q 1
#if 0
int unreached_q = 0;
#elifdef ELIFDEF_Q
int from_elifdef = 1;
#endif
#if 0
int unreached_r = 0;
#elifndef ELIFDEF_R
int from_elifndef = 2;
#endif
int a = 1;
