// Record conditional groups from the preprocessor's own directive events.
//
// Every conditional directive Clang reads is reported through
// PPCallbacks::ConditionalDirective, including the ones it only scans past in
// an excluded block, where none of If/Elif/Else/Endif fires.  Each arm records
// its directive's exact extent and what the preprocessor did with it: `taken`
// when it entered the arm, `evaluated` when it decided the arm at all.  The
// first arm below is taken but prints no token, so it has no `pp_span`.

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -E -P --refold-map=%t/map.json %t/conds.c -o %t/out.i
// RUN: FileCheck --input-file=%t/map.json %s

// An arm after the taken one is read but never evaluated.

// CHECK:      "conds": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "id": 0,
// CHECK-NEXT:     "file": "{{.*}}conds.c",
// CHECK-NEXT:     "group_b": 0,
// CHECK-NEXT:     "group_e": 83,
// CHECK-NEXT:     "endif_b": 76,
// CHECK-NEXT:     "endif_e": 83,
// CHECK-NEXT:     "arms": [
// CHECK-NEXT:       {
// CHECK-NEXT:         "id": 0,
// CHECK-NEXT:         "kind": "if",
// CHECK-NEXT:         "cond": "1\n",
// CHECK-NEXT:         "body_b": 6,
// CHECK-NEXT:         "body_e": 30,
// CHECK-NEXT:         "directive_b": 0,
// CHECK-NEXT:         "directive_e": 6,
// CHECK-NEXT:         "taken": true,
// CHECK-NEXT:         "evaluated": true
// CHECK-NEXT:       },
// CHECK-NEXT:       {
// CHECK-NEXT:         "id": 1,
// CHECK-NEXT:         "kind": "elif",
// CHECK-NEXT:         "cond": "2\n",
// CHECK-NEXT:         "body_b": 38,
// CHECK-NEXT:         "body_e": 54,
// CHECK-NEXT:         "directive_b": 30,
// CHECK-NEXT:         "directive_e": 38,
// CHECK-NEXT:         "taken": false,
// CHECK-NEXT:         "evaluated": false

// A group nested in an excluded arm fires none of the per-directive callbacks,
// and is recorded all the same, under the arm that excluded it.

// CHECK:          "kind": "ifdef",
// CHECK-NEXT:     "cond": "UNDEFINED_NAME\n",
// CHECK:          "taken": false,
// CHECK-NEXT:     "evaluated": true
// CHECK:          "id": 2,
// CHECK-NEXT:     "file": "{{.*}}conds.c",
// CHECK-NEXT:     "parent_arm_id": 3,
// CHECK-NEXT:     "group_b": 105,
// CHECK-NEXT:     "group_e": 164,
// CHECK-NEXT:     "endif_b": 157,
// CHECK-NEXT:     "endif_e": 164,
// CHECK:          "directive_b": 105,
// CHECK-NEXT:     "directive_e": 111,
// CHECK-NEXT:     "taken": false,
// CHECK-NEXT:     "evaluated": false

// A digraph introducer is a directive like any other.

// CHECK:          "group_b": 171,
// CHECK-NEXT:     "group_e": 205,
// CHECK:          "directive_b": 171,
// CHECK-NEXT:     "directive_e": 178,
// CHECK-NEXT:     "taken": true,

// A comment before the `#` does not stop it being a directive, and a comment
// crossing a newline continues the directive onto the next line.

// CHECK:          "group_b": 216,
// CHECK-NEXT:     "group_e": 275,
// CHECK:          "cond": "0 /* spans\n  a newline */\n",
// CHECK:          "directive_b": 216,
// CHECK-NEXT:     "directive_e": 246,

//--- conds.c
#if 1
#define ONLY_A_DEFINE 1
#elif 2
int never_elif;
#else
int never_else;
#endif
#ifdef UNDEFINED_NAME
#if 1
int nested_in_skipped;
#else
int nested_else;
#endif
#endif
%:if 1
int digraph_taken;
%:endif
/* note */ #if 0 /* spans
  a newline */
int comment_prefixed;
#endif
int tail;
