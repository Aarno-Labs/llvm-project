// Record the complete physical extent of a macro-state directive line.
//
// The refold map's directive "text" is rendered from the parsed MacroInfo with
// canonical single-space separation, so for a backslash-continued definition it
// is neither the source bytes nor any transform of them.  [site_b, site_e) is
// anchored on the macro name and stops at the first physical newline, so it
// does not describe the directive either.  The map therefore also records
// [directive_line_b, directive_line_e): the directive from its `#` to where
// Clang's lexer stands after reading the end-of-directive token.
//
// The `#define` below occupies bytes [0,46) of continued.c across three
// physical lines, while its name-anchored site range is only [8,27).

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -E -P --refold-map=%t/map.json %t/continued.c -o %t/out.i
// RUN: FileCheck --input-file=%t/map.json %s
// RUN: %clang_cc1 -E -P --refold-map=%t/comments.json %t/comments.c \
// RUN:   -o %t/comments.i
// RUN: FileCheck --check-prefix=COMMENTS --input-file=%t/comments.json %s
// RUN: %clang_cc1 -E -P -ftrigraphs --refold-map=%t/trigraph.json \
// RUN:   %t/trigraph.c -o %t/trigraph.i 2>/dev/null
// RUN: FileCheck --check-prefix=TRIGRAPH --input-file=%t/trigraph.json %s

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

// The extent ends where Clang's lexer stands after the end-of-directive token,
// so a block comment that crosses a newline continues the directive.  A scan
// of the bytes for the first unspliced newline ended TRAILING's extent inside
// its comment, at 34, and found no extent at all for the #undef, whose name
// follows the comment.

// COMMENTS:      "name": "TRAILING",
// COMMENTS:      "directive_line_b": 0,
// COMMENTS-NEXT: "directive_line_e": 56,

// COMMENTS:      "subkind": "#undef",
// COMMENTS-NEXT: "name": "TRAILING",
// COMMENTS-NEXT: "text": "#undef TRAILING\n",
// COMMENTS-NEXT: "site_b": 115,
// COMMENTS-NEXT: "site_e": 124,
// COMMENTS-NEXT: "site_path":
// COMMENTS-NEXT: "directive_line_b": 74,
// COMMENTS-NEXT: "directive_line_e": 124,

// A trigraph `??/` before a newline is a line splice when trigraphs are
// enabled, so TRI's definition continues onto its second line.

// TRIGRAPH:      "name": "TRI",
// TRIGRAPH:      "directive_line_b": 0,
// TRIGRAPH-NEXT: "directive_line_e": 24,

//--- continued.c
#define CONTINUED(a, b)		\
	((a) +			\
	 (b))
int x = CONTINUED(1, 2);
#undef CONTINUED

//--- comments.c
#define TRAILING 1 /* a note that
   spans two lines */
int y = TRAILING;
#undef /* between keyword
   and name */ TRAILING

//--- trigraph.c
#define TRI 1 ??/
  + 2
int t = TRI;
