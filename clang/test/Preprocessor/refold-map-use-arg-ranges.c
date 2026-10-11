// Record where a call formed during another expansion read an actual from
// source.
//
// Such a call reads the rest of the expansion that formed it and then the file,
// so an actual can be spelled in the file after the callsite of the macro that
// formed the call, outside any invocation text: `CALL(ADD, 1), 2)` leaves
// `ADD(1` open and reads `, 2)`.  use_arg_ranges gives, per formal, the bytes
// of such an actual in the use's file instance (the coordinates of use_b and
// use_e), or nulls when a macro produced any of its tokens.  A copy of one of
// those tokens is credited to its formal in arg_spans, whichever invocation it
// is primarily attributed to.

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -E -P -I%t --refold-map=%t/map.json %t/main.c -o %t/out.i
// RUN: FileCheck --input-file=%t/map.json %s

// CHECK: "version": "3.14",

// ADD's `b` is the `2` after CALL's callsite; its `a` came from CALL.

// CHECK:      "inv_text": "ADD, 1), 2)",
// CHECK:      "use_b": 185,
// CHECK-NEXT: "use_e": 201,
// CHECK-NEXT: "use_arg_ranges": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "b": null,
// CHECK-NEXT:     "e": null
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "b": 199,
// CHECK-NEXT:     "e": 200
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_spans": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "begin": 5,
// CHECK-NEXT:     "end": 6,
// CHECK-NEXT:     "arg_index": 1,

// An actual that MIX's expansion began and the source continued, `1 + 3`, has
// a token a macro produced, so only `4` is located.

// CHECK:      "inv_text": "ADD, 1) + 3, 4)",
// CHECK:      "use_arg_ranges": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "b": null,
// CHECK-NEXT:     "e": null
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "b": 220,
// CHECK-NEXT:     "e": 221
// CHECK-NEXT:   }
// CHECK-NEXT: ],

// HF's name was spelled in the header, but `(t)` was read from main.c, which
// use_b, use_e and use_arg_ranges index.

// CHECK:      "inv_text": null,
// CHECK-NEXT: "inv_file": "{{.*}}h.h",
// CHECK:      "use_b": 235,
// CHECK-NEXT: "use_e": 244,
// CHECK-NEXT: "use_arg_ranges": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "b": 242,
// CHECK-NEXT:     "e": 243
// CHECK-NEXT:   }
// CHECK-NEXT: ],

// DO_ADD's name was pasted, so it records no invocation text, and the copy of
// `10` that STAGE printed is attributed to GET.  It is still DO_ADD's `x`.

// CHECK:      "inv_text": null,
// CHECK-NEXT: "use_b": 253,
// CHECK-NEXT: "use_e": 265,
// CHECK-NEXT: "use_arg_ranges": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "b": 262,
// CHECK-NEXT:     "e": 264
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "arg_spans": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "begin": 30,
// CHECK-NEXT:     "end": 31,
// CHECK-NEXT:     "arg_index": 0,

// A call spelled in source records its actuals in inv_arg_ranges only.

// CHECK:      "inv_text": "F(1)",
// CHECK-NOT:  "use_arg_ranges"
// CHECK:      "tokmap": [

//--- h.h
#define HF(v) int hf_##v = v;
#define HALIAS HF
//--- main.c
#include "h.h"
#define ADD(a, b) a + b
#define CALL(f, a) int k = f(a
#define MIX(f, a) int m = f(a
#define GET(t) DO_##t
#define DO_ADD(x) STAGE(x)
#define STAGE(x) (x)
#define F(v) v
CALL(ADD, 1), 2);
MIX(ADD, 1) + 3, 4);
int t = 0;
HALIAS(t)
int g = GET(ADD)(10);
int f = F(1);
