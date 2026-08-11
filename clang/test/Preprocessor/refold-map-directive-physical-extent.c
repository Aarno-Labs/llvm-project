// Record the complete physical extent of a macro-state directive line.
//
// The refold map's directive "text" is rendered from the parsed MacroInfo with
// canonical single-space separation, so for a backslash-continued definition it
// is neither the source bytes nor any transform of them.  [site_b, site_e) is
// anchored on the macro name and stops at the first physical newline, so it
// does not describe the directive either.  The map therefore also records
// [directive_line_b, directive_line_e): the directive from its `#` through the
// end of the logical line reached after every phase-two splice.
//
// The `#define` below occupies bytes [0,46) of continued.c across three
// physical lines, while its name-anchored site range is only [8,27).

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -E -P --refold-map=%t/map.json %t/continued.c -o %t/out.i
// RUN: FileCheck --input-file=%t/map.json %s

// CHECK:      "name": "CONTINUED",
// CHECK-NEXT: "text": "#define CONTINUED(a,b) ((a) + (b))\n",
// CHECK-NEXT: "site_b": 8,
// CHECK-NEXT: "site_e": 27,
// CHECK-NEXT: "site_path":
// CHECK-NEXT: "directive_line_b": 0,
// CHECK-NEXT: "directive_line_e": 46,

// An uncontinued directive reports the same extent both ways, apart from the
// leading `#` that site_b skips.

// CHECK:      "subkind": "#undef",
// CHECK-NEXT: "name": "CONTINUED",
// CHECK-NEXT: "text": "#undef CONTINUED\n",
// CHECK-NEXT: "site_b": 78,
// CHECK-NEXT: "site_e": 88,
// CHECK-NEXT: "site_path":
// CHECK-NEXT: "directive_line_b": 71,
// CHECK-NEXT: "directive_line_e": 88,

//--- continued.c
#define CONTINUED(a, b)		\
	((a) +			\
	 (b))
int x = CONTINUED(1, 2);
#undef CONTINUED
