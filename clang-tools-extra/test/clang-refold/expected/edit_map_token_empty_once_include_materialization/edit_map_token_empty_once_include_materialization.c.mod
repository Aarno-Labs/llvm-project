// Regression: a `#pragma once` re-inclusion the preprocessor never entered
// contributes no A token, so the producer records it with no preprocessed span
// and its `PPCover` is invalid.  The pragma-once machinery still forces that
// occurrence to be materialized, because its closure re-enters an already
// inlined once-header.  The resulting edit therefore has no A-token cover to
// project into B and no sideband replay envelope, and under `--emit-edit-map`
// it used to reach emission with no B-side materialization range at all, which
// condemned the whole translation unit to a verbatim copy of the edited stream.
//
// The occurrence realizes no B bytes -- it emits preprocessor state only -- so
// it is certified B-payload-free and omitted from the edit map, exactly like a
// copied original slice.  An edit that reaches emission with no certificate at
// all still fails closed.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir/headers
// RUN: cp %s %t.dir/edit_map_token_empty_once_include_materialization.c
// RUN: cp %S/headers/edit_map_token_empty_once/*.h %t.dir/headers/
//
// RUN: cd %t.dir && clang -E -P -I headers --refold-map=%t.dir/edit_map_token_empty_once_include_materialization.c.refold.json edit_map_token_empty_once_include_materialization.c -o %t.dir/edit_map_token_empty_once_include_materialization.c.i
// RUN: diff -u %S/expected/edit_map_token_empty_once_include_materialization/edit_map_token_empty_once_include_materialization.c.i %t.dir/edit_map_token_empty_once_include_materialization.c.i
//
// RUN: cd %t.dir && clang-refold --no-lines --strict --verify-output=fatal --log-level=trace --pp %t.dir/edit_map_token_empty_once_include_materialization.c.i --pp-mod %S/expected/edit_map_token_empty_once_include_materialization/edit_map_token_empty_once_include_materialization.c.i.mod --refold-map %t.dir/edit_map_token_empty_once_include_materialization.c.refold.json --out %t.dir/edit_map_token_empty_once_include_materialization.c.mod --emit-edit-map %t.dir/edit_map_token_empty_once_include_materialization.editmap.json 2>&1 | FileCheck %s
// RUN: diff -u %S/expected/edit_map_token_empty_once_include_materialization/edit_map_token_empty_once_include_materialization.c.mod %t.dir/edit_map_token_empty_once_include_materialization.c.mod
//
// RUN: cd %t.dir && clang-refold --no-lines --strict --log-level=trace --check %t.dir/edit_map_token_empty_once_include_materialization.c.mod --pp-mod %S/expected/edit_map_token_empty_once_include_materialization/edit_map_token_empty_once_include_materialization.c.i.mod --refold-map %t.dir/edit_map_token_empty_once_include_materialization.c.refold.json

// The token-empty occurrence is the second `#include "edit_map_once_mid.h"`:
// `edit_map_once_parent.h` already entered that header, so this one is skipped.
// CHECK: forcing materialization of inc#{{[0-9]+}} target='"edit_map_once_mid.h"'
// CHECK: B-payload-free edit in {{.*}} emits {{[0-9]+}} byte(s) of preprocessor state and is omitted from the materialized edit map

// The unit refolds rather than emitting the edited stream verbatim.
// CHECK: refold summary [production attempt 0]: {{.*}} terminalFallback=no

#ifndef __CLANG_REFOLD_ONCE_2
#define __CLANG_REFOLD_ONCE_2
#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
int eom_leaf_value = 99;
#endif
int eom_mid_value = 22;
#endif
// A second occurrence of the leaf, suppressed by its `#pragma once`. Without
// it the leaf has a single recorded occurrence and takes the delete-pragma
// treatment, which is a different theorem from the one under test.
#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#include "edit_map_once_leaf.h"
#endif
int eom_parent_value = 33;
#ifndef __CLANG_REFOLD_ONCE_2
#define __CLANG_REFOLD_ONCE_2
#ifndef __CLANG_REFOLD_ONCE_1
#define __CLANG_REFOLD_ONCE_1
#include "edit_map_once_leaf.h"
#endif
int eom_mid_value = 22;
#endif
int eom_tail = 44;
