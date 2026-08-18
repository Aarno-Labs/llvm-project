// RUN: %clang-refold-tester macro_state_gap_observed_name_defers_to_liveness_planner
// Precedence regression: an observed macro name keeps its seam with the
// macro-state liveness planner.
//
// Identical to the straddling-`#define` case except that `VALUE` is used after
// the directive.  The payload is still one token naming no identifier, so the
// placement obligation is still discharged -- but discharging it is not
// authority to take the seam.  The liveness planner is the only stage that
// models resurrection, undef/restore and replay ordering for a consumed
// directive, and it engages exactly when the name still matters.  The tiler
// must therefore leave this one alone.
//
// This pins the precedence rule rather than a proof.  Without it, a later
// completeness change can widen the tiler's macro-state exemption and silently
// take every such seam: the outputs would still replay B and the suite would
// stay green, while the tests named for the liveness planner stopped reaching
// it at all.
int arr[] = { 1,
#define VALUE 3
2 };
int used = VALUE;
