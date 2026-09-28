// Record a pragma once per inclusion of the header that spells it.
//
// A header entered twice shares its path and byte offsets between the two
// entries.  Keyed on those alone, the second entry's pragma folded into the
// first entry's record: that record kept only the first owner and the first
// image, the second print had no record, and `pragma_images_complete` was false
// for the whole translation unit.  The file instance distinguishes the two.

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -E -P -I%t --refold-map=%t/map.json %t/tu.c -o %t/out.i
// RUN: FileCheck --input-file=%t/map.json %s

// CHECK:      "pragma_images_complete": true,

// Items are numbered in the order they are recorded: the first `#include`, its
// pragma, the second `#include`, its pragma.  Each pragma record names its own
// inclusion as owner and carries its own image.

// CHECK:      "text": "#pragma vendor note\n",
// CHECK-NEXT: "site_b": 0,
// CHECK-NEXT: "site_e": 20,
// CHECK-NEXT: "site_path": "{{.*}}twice.h",
// CHECK-NEXT: "pp_byte_begin": 0,
// CHECK-NEXT: "pp_byte_end": 19,
// CHECK-NEXT: "owner_include_id": [[#FIRST:]]

// CHECK:      "id": [[#FIRST + 2]],
// CHECK-NEXT: "kind": "directive",
// CHECK-NEXT: "subkind": "#include",

// CHECK:      "text": "#pragma vendor note\n",
// CHECK-NEXT: "site_b": 0,
// CHECK-NEXT: "site_e": 20,
// CHECK-NEXT: "site_path": "{{.*}}twice.h",
// CHECK-NEXT: "pp_byte_begin": 20,
// CHECK-NEXT: "pp_byte_end": 39,
// CHECK-NEXT: "owner_include_id": [[#FIRST + 2]]

//--- tu.c
#include "twice.h"
#include "twice.h"
int x;
//--- twice.h
#pragma vendor note
