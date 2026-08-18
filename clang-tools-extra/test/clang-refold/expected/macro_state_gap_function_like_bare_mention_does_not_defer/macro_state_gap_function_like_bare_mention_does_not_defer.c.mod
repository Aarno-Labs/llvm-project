// RUN: %clang-refold-tester macro_state_gap_function_like_bare_mention_does_not_defer
// Precedence regression: a bare mention of a function-like macro after the gap
// is not an observation, so the seam stays the tiler's.
//
// `macro_state_gap_observed_name_defers_to_liveness_planner` pins the other
// direction: when something after the gap still observes the directive, the
// macro-state liveness planner owns the seam, because it is the only stage
// modelling resurrection, undef/restore and replay ordering.  That deference
// is decided by whether the name is *observed*, and observation is a property
// of the definition's shape, not of the spelling: a function-like macro is
// reached only by NAME followed by `(`.
//
// Here `VALUE` is function-like and the surviving mention is bare, so nothing
// after the gap reaches the definition and the planner has no ordering to
// decide.  The tiler splits at the preserved directive, which keeps its line,
// and commits the payload after it.
int arr[] = { 
#define VALUE(x) (x)
9 };
int bare = VALUE;
