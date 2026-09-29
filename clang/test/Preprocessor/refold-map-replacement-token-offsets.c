// Record where each replacement-list token of a #define is spelled.
//
// The refold map's directive "text" is rendered from the parsed MacroInfo with
// canonical spacing, so none of its offsets name a source byte once the
// definition uses extra blanks, comments or backslash continuations.  Each
// replacement_tokens entry therefore carries [site_b, site_e): the token's
// bytes in site_path.  The ranges are recorded for every token or for none,
// and only inside a recorded directive_line_b/directive_line_e extent.
//
// PAIR's body `a /* sum */ + \⏎	b` puts `a` at 36, `+` at 48 and `b` at 53.

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -E -P --refold-map=%t/map.json %t/tokens.c -o %t/out.i
// RUN: FileCheck --input-file=%t/map.json %s

// An empty replacement list is still emitted, so an absent tape can never be
// read as an empty one.

// CHECK:      "name": "EMPTY",
// CHECK:      "function_like": false,
// CHECK-NEXT: "replacement_tokens": [],

// CHECK:      "name": "PAIR",
// CHECK:      "directive_line_b": 14,
// CHECK-NEXT: "directive_line_e": 55,
// CHECK:      "replacement_tokens": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "kind": "param_ref",
// CHECK-NEXT:     "param_index": 0,
// CHECK-NEXT:     "spelling": "a",
// CHECK-NEXT:     "site_b": 36,
// CHECK-NEXT:     "site_e": 37
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "kind": "literal",
// CHECK-NEXT:     "spelling": "+",
// CHECK-NEXT:     "site_b": 48,
// CHECK-NEXT:     "site_e": 49
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "kind": "param_ref",
// CHECK-NEXT:     "param_index": 1,
// CHECK-NEXT:     "spelling": "b",
// CHECK-NEXT:     "site_b": 53,
// CHECK-NEXT:     "site_e": 54
// CHECK-NEXT:   }
// CHECK-NEXT: ],

// The physical-extent scan does not model a block comment that crosses a
// newline, so SPLIT has no extent, and its tokens get no ranges either.

// CHECK:      "name": "SPLIT",
// CHECK-NOT:  "directive_line_b"
// CHECK:      "replacement_tokens": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "kind": "param_ref",
// CHECK-NEXT:     "param_index": 0,
// CHECK-NEXT:     "spelling": "x"
// CHECK-NEXT:   }
// CHECK-NEXT: ],

//--- tokens.c
#define EMPTY
#define PAIR( a ,b )  a /* sum */ + \
	b
#define SPLIT(x) /* crosses
   a newline */ x
int v = PAIR(1, 2) EMPTY + SPLIT(3);
