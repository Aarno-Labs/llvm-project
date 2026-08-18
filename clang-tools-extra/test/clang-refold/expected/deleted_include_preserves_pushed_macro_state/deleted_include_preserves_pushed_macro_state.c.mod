// RUN: %clang-refold-tester deleted_include_preserves_pushed_macro_state
// Deleting every token a header contributed must still keep that header's
// `#pragma push_macro`: the translation unit pops it after the include has
// ended, so dropping the push would leave the pop restoring nothing and the
// second use would diverge.  `#undef` and `#define` in the same header are
// preserved for the same reason.
//
// The alignment only reaches that shape after a repair.  The owner-depth
// tie-break is an additive per-deleted-token cost, so it first selects the
// deletion run shifted one token into the translation unit -- deleting the
// TU's `int` costs less than deleting the header's.  No single owner covers
// that run, `OwnerClosedCover` fails, and the attempt asks for the terminal
// carrier.  The ladder then moves the run back onto the header's own cover,
// where the include materializes and every directive survives.
#define STATE_PUSHED 1
#pragma push_macro("STATE_PUSHED")
#undef STATE_PUSHED
#define STATE_PUSHED 2

int state_pushed_inner = STATE_PUSHED;
#pragma pop_macro("STATE_PUSHED")
int state_pushed_outer = STATE_PUSHED;
