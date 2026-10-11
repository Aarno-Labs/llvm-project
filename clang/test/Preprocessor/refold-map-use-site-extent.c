// Record each macro call's use, not only where its name was spelled.
//
// [inv_b, inv_e) begins where the call's name was spelled, which for a name a
// macro produced is a replacement list or a caller's actual, perhaps in a
// #define far before the call, or in another file.  [use_b, use_e) is the use
// in the file instance being lexed: from where the outermost expansion its name
// came from was used to the end of its last token.  Every source token the
// call read lies inside it.

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -E -P -I%t --refold-map=%t/map.json %t/main.c -o %t/out.i
// RUN: FileCheck --input-file=%t/map.json %s

// A call spelled in a header is used in that header's instance.

// CHECK:      "inv_text": "HF(h)",
// CHECK-NEXT: "inv_file": "{{.*}}h.h",
// CHECK-NEXT: "inv_b": 44,
// CHECK-NEXT: "inv_e": 49,
// CHECK-NEXT: "use_b": 44,
// CHECK-NEXT: "use_e": 49,

// A call spelled in the source is its own use.

// CHECK:      "inv_text": "F(1)",
// CHECK-NEXT: "inv_file": "{{.*}}main.c",
// CHECK-NEXT: "inv_b": 39,
// CHECK-NEXT: "inv_e": 43,
// CHECK-NEXT: "use_b": 39,
// CHECK-NEXT: "use_e": 43,

// The name comes from OBJ, which ends before `(x)`: the use is `OBJ(x)`, while
// [inv_b, inv_e) runs from OBJ's #define over `int z;`.

// CHECK:      "inv_text": "OBJ",
// CHECK:      "use_b": 98,
// CHECK-NEXT: "use_e": 101,
// CHECK:      "inv_text": "F2\nint z;\nOBJ(x)",
// CHECK-NEXT: "inv_file": "{{.*}}main.c",
// CHECK-NEXT: "inv_b": 88,
// CHECK-NEXT: "inv_e": 104,
// CHECK-NEXT: "use_b": 98,
// CHECK-NEXT: "use_e": 104,

// A call wholly inside OUTER's replacement list is used where OUTER is.

// CHECK:      "inv_text": "F(v)",
// CHECK-NEXT: "inv_file": "{{.*}}main.c",
// CHECK-NEXT: "inv_b": 122,
// CHECK-NEXT: "inv_e": 126,
// CHECK-NEXT: "use_b": 127,
// CHECK-NEXT: "use_e": 135,

// G's name comes from CALL's actual and its `)` from the source after CALL's
// callsite: the use is `CALL(G, y))`.

// CHECK:      "inv_text": "G, y))",
// CHECK-NEXT: "inv_file": "{{.*}}main.c",
// CHECK-NEXT: "inv_b": 194,
// CHECK-NEXT: "inv_e": 200,
// CHECK-NEXT: "use_b": 189,
// CHECK-NEXT: "use_e": 200,

// A header alias used in the source: inv_b is in the header and inv_e in the
// source, while the use is `HALIAS(t)` in the source.

// CHECK:      "name": "HF",
// CHECK-NEXT: "inv_text": null,
// CHECK-NEXT: "inv_file": "{{.*}}h.h",
// CHECK-NEXT: "inv_b": 41,
// CHECK-NEXT: "inv_e": 211,
// CHECK-NEXT: "use_b": 202,
// CHECK-NEXT: "use_e": 211,

//--- h.h
#define HF(v) int hf_##v;
#define HALIAS HF
HF(h)
//--- main.c
#include "h.h"
#define F(v) int f_##v;
F(1)
#define F2(a) int a; int g_##a;
#define OBJ F2
int z;
OBJ(x)
#define OUTER(v) F(v)
OUTER(2)
#define G(v) g_##v
#define CALL(f, a) int a; int f(a
CALL(G, y));
HALIAS(t)
