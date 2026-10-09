// Record arg_deps only where scanning the spelled arguments for the caller's
// formal names is a proof.
//
// An empty arg_deps entry tells the consumer that no caller formal reaches the
// argument.  A scan of inv_text for formal names establishes that only when
// the call was collected, name through closing parenthesis, from one buffer
// that is not a substituted actual, every argument range was delimited, and
// every identifier is spelled as its name.  Otherwise arg_deps and arg_refs
// are both absent.

// RUN: %clang_cc1 -E -P --refold-map=%t.json %s -o %t.i
// RUN: FileCheck --input-file=%t.json %s

// Spelled in the caller's replacement list, so the scan sees `X`.
#define BODY_CALLEE(v) int body_##v;
#define BODY_CALLER(X) BODY_CALLEE(X)
BODY_CALLER(a)

// CHECK:      "inv_text": "BODY_CALLEE(X)",
// CHECK:      "callee_origin": {
// CHECK:      "arg_deps": [
// CHECK-NEXT:   [
// CHECK-NEXT:     0
// CHECK-NEXT:   ]
// CHECK-NEXT: ],

// Spelled in the caller's actual and expanded before the caller substituted
// anything, so no caller formal reaches it and the empty set is a proof.
#define ACTUAL_CALLEE(v) int actual_##v;
#define ACTUAL_CALLER(X) X
ACTUAL_CALLER(ACTUAL_CALLEE(b))

// CHECK:      "inv_text": "ACTUAL_CALLEE(b)",
// CHECK:      "callee_origin": {
// CHECK:      "arg_deps": [
// CHECK-NEXT:   []
// CHECK-NEXT: ],

// Collected from the caller's replacement list and the caller's actual, so
// inv_text runs from the #define to the callsite and no argument range is
// delimited.  `c` came through PAIR.
#define RANGE_CALLEE(X) int range_##X;
#define RANGE_CALLER(PAIR) RANGE_CALLEE PAIR
RANGE_CALLER((c))

// CHECK:      "inv_text": "RANGE_CALLEE PAIR\nRANGE_CALLER((c)",
// CHECK:      "inv_arg_ranges": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "b": null,
// CHECK-NEXT:     "e": null
// CHECK-NEXT:   }
// CHECK-NEXT: ],
// CHECK:      "callee_origin": {
// CHECK-NOT:  "arg_deps"
// CHECK-NOT:  "arg_refs"
// CHECK:      "spans": [

// Named in the caller's replacement list but given its argument list by the
// caller's actual, so inv_text runs from the #define to the callsite and the
// argument range is parsed out of the unrelated `(void)` between them.
#define SPLIT_CALLEE(v) int split_##v;
#define SPLIT_CALLER(P) SPLIT_CALLEE P
int split_f(void);
SPLIT_CALLER((h))

// CHECK:      "inv_text": "SPLIT_CALLEE P\nint split_f(void);\nSPLIT_CALLER((h)",
// CHECK:      "callee_origin": {
// CHECK-NOT:  "arg_deps"
// CHECK-NOT:  "arg_refs"
// CHECK:      "spans": [

// Collected from two substituted actuals.  The envelope
// `TUPLE_CALLEE, (d, e)` delimits `d` and `e`, but they came through PAIR,
// which the text does not name.
#define TUPLE_CALLEE(p, q) int p; int q;
#define TUPLE_CALLER(F, PAIR) F PAIR
TUPLE_CALLER(TUPLE_CALLEE, (d, e))

// CHECK:      "inv_text": "TUPLE_CALLEE, (d, e)",
// CHECK:      "callee_origin": {
// CHECK-NOT:  "arg_deps"
// CHECK-NOT:  "arg_refs"
// CHECK:      "spans": [

// Collected from one substituted actual and invoked on rescan after a paste,
// so its tokens came through `x` although the text `RESCAN_CALLEE(f)` names
// no formal.
#define RESCAN_CALLEE(v) int rescan_##v;
#define RESCAN_CALLER(x, y) x ## y
RESCAN_CALLER(RESCAN_CALLEE(f), )

// CHECK:      "inv_text": "RESCAN_CALLEE(f)",
// CHECK:      "callee_origin": {
// CHECK-NOT:  "arg_deps"
// CHECK-NOT:  "arg_refs"
// CHECK:      "spans": [

// Names the formal `ab` across a line splice, which a comparison of raw
// spellings would miss.
#define SPLICE_CALLEE(v) int splice_##v;
#define SPLICE_CALLER(ab) SPLICE_CALLEE(a\
b)
SPLICE_CALLER(g)

// CHECK:      "inv_text": "SPLICE_CALLEE(a\\\nb)",
// CHECK:      "callee_origin": {
// CHECK-NOT:  "arg_deps"
// CHECK-NOT:  "arg_refs"
// CHECK:      "spans": [
