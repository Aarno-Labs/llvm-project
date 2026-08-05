// RUN: %clang-refold-tester deleted_include_preserves_pushed_macro_state
// XFAIL: *
// Known gap: `#pragma push_macro` is consumed by the preprocessor and emits no
// tokens, so deleting everything the include contributed drops the push while
// the translation unit's `#pragma pop_macro` survives.  The pop then restores
// nothing and the second use diverges.  `#undef` and `#define` in the same
// header are preserved; this state kind is not.
//
// The closing output check catches it, so `--verify-output=fatal` refuses and
// `--verify-output=repair` falls back to expansion.  At the default `off` the
// wrong source is emitted silently, which is why this is recorded rather than
// left to be rediscovered.
#define STATE_PUSHED 1
#pragma push_macro("STATE_PUSHED")
#undef STATE_PUSHED
#define STATE_PUSHED 2
int state_pushed_inner = STATE_PUSHED;
#pragma pop_macro("STATE_PUSHED")
int state_pushed_outer = STATE_PUSHED;
