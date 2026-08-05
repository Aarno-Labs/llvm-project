// RUN: %clang-refold-tester deleted_include_preserves_consumed_undef
// The mirror of the `#define` case: the header's `#undef` is the reason the
// surviving use stays an identifier.  Dropping the include would resurrect the
// translation unit's earlier definition and silently change the use.
#define STATE_UNDEFINED 1
#undef STATE_UNDEFINED
int state_undef_use = STATE_UNDEFINED;
