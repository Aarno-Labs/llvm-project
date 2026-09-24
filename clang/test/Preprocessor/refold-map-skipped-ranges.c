// Record the source ranges the preprocessor skipped in excluded conditional
// groups, per file instance.
//
// A conditional arm record's "selected" flag is a byte-overlap test against the
// token map, not a takenness fact.  "skipped_ranges" is Clang's own
// SourceRangeSkipped callback: [b, e) runs from the `#` of the directive that
// started skipping to the end of the directive that stopped it, and nothing in
// between produced a token or changed preprocessor state.
//
// In main.c the `#ifdef NOPE` arm is skipped as [7,41), and so is the nested
// `#ifdef ALSO_NOPE` arm inside the taken `#else`, as [44,87).  Neither has an
// owner include.

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -E -P --refold-map=%t/map.json %t/main.c -o %t/out.i
// RUN: FileCheck --input-file=%t/map.json %s

// CHECK:      "skipped_ranges": [
// CHECK-NEXT:   {
// CHECK-NEXT:     "physical_file": "{{.*}}main.c",
// CHECK-NEXT:     "b": 7,
// CHECK-NEXT:     "e": 41
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "physical_file": "{{.*}}main.c",
// CHECK-NEXT:     "b": 44,
// CHECK-NEXT:     "e": 87
// CHECK-NEXT:   },

// The same header included twice skips a different arm in each instance, so
// each range carries the include occurrence that lexed it: the first instance
// skips `#ifdef TWICE` [0,39), the second skips `#else` [34,80).

// CHECK-NEXT:   {
// CHECK-NEXT:     "physical_file": "{{.*}}twice.h",
// CHECK-NEXT:     "b": 0,
// CHECK-NEXT:     "e": 39,
// CHECK-NEXT:     "owner_include_id": [[FIRST:[0-9]+]]
// CHECK-NEXT:   },
// CHECK-NEXT:   {
// CHECK-NEXT:     "physical_file": "{{.*}}twice.h",
// CHECK-NEXT:     "b": 34,
// CHECK-NEXT:     "e": 80,
// CHECK-NEXT:     "owner_include_id": [[SECOND:[0-9]+]]
// CHECK-NEXT:   }
// CHECK-NEXT: ],

// CHECK:      "id": [[FIRST]],
// CHECK-NEXT: "kind": "directive",
// CHECK-NEXT: "subkind": "#include",
// CHECK-NEXT: "text": "#include \"twice.h\"\n",
// CHECK-NEXT: "site_b": 117,
// CHECK:      "id": [[SECOND]],
// CHECK-NEXT: "kind": "directive",
// CHECK-NEXT: "subkind": "#include",
// CHECK-NEXT: "text": "#include \"twice.h\"\n",
// CHECK-NEXT: "site_b": 136,

//--- main.c
int a;
#ifdef NOPE
int skipped_one;
#else
  #ifdef ALSO_NOPE
  int skipped_two;
  #else
  int taken;
  #endif
#endif
#include "twice.h"
#include "twice.h"
//--- twice.h
#ifdef TWICE
int second_instance;
#else
#define TWICE
int first_instance;
#endif
