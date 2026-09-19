// RUN: %clang-refold-tester-verify-off macro_state_undef_restore_per_edit_two_payloads
// Two B payloads reading the same live macro are each bracketed by their own
// `#undef` and restore.
//
// The repair used to be recorded per definition, and a definition repaired
// once was skipped both by the repair and by the liveness audit for every
// later edit, so the second payload would have been emitted under the live
// definition.  With a restore after each payload the repairs are independent,
// and the audit reads each replacement's own text.
#define TWO_ZZ 3
int two_b = TWO_ZZ;
int two_p = 1;
int two_m = TWO_ZZ;
int two_q = 2;
int two_later = TWO_ZZ;
