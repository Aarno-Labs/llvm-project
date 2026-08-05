// RUN: env CLANG_REFOLD_TEST_ONLY_FORCE_EXPAND_INCLUDE=forced_expand_inner.h %clang-refold-tester verify_repair_expands_marked_nested_include
// Regression: the narrowed region can be an include the translation unit never
// names.  A divergence inside a header reached through another header owns that
// inner include, and expanding it requires the enclosing include to be
// materialized too -- otherwise there is nowhere to put the inner body.
//
// The ancestor pull-in is the materialization scheduler's own, not something
// narrowing arranges: marking the inner include is enough, and the outer one
// follows because a seeded include always brings its ancestors.
int inner_value = 12;
int outer_value = 11;
int tu_value = 14;
