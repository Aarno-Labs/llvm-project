// Record the complete physical extent of an include directive, and the exact
// bytes of its keyword and header-name operand.
//
// An include item's "text" is synthesized as `#include "name"`, and
// [site_b, site_e) stops at the first physical newline, so neither describes a
// directive spelled with extra blanks, a comment, or a splice.  The map
// therefore also records [directive_line_b, directive_line_e), measured from
// the `#` to where Clang's lexer stands after the end-of-directive token, and
// Clang's own keyword and operand tokens inside it.

// RUN: rm -rf %t && split-file %s %t
// RUN: %clang_cc1 -E -P -I%t --refold-map=%t/map.json %t/inc.c -o %t/out.i
// RUN: FileCheck --input-file=%t/map.json %s

// Blanks and a trailing comment: the extent is the whole physical line.

// CHECK:      "text": "#include \"leaf.h\"\n",
// CHECK-NEXT: "site_b": 0,
// CHECK-NEXT: "site_e": 44,
// CHECK:      "angled": false,
// CHECK-NEXT: "directive_line_b": 0,
// CHECK-NEXT: "directive_line_e": 44,
// CHECK-NEXT: "keyword_b": 1,
// CHECK-NEXT: "keyword_e": 8,
// CHECK-NEXT: "operand_b": 10,
// CHECK-NEXT: "operand_e": 18,

// A block comment that crosses a newline continues the directive, which a
// physical-line or splice scan cannot see.

// CHECK:      "site_b": 44,
// CHECK-NEXT: "site_e": 62,
// CHECK:      "angled": false,
// CHECK-NEXT: "directive_line_b": 44,
// CHECK-NEXT: "directive_line_e": 83,
// CHECK-NEXT: "keyword_b": 45,
// CHECK-NEXT: "keyword_e": 52,
// CHECK-NEXT: "operand_b": 74,
// CHECK-NEXT: "operand_e": 82,

// A splice between the introducer and the keyword.

// CHECK:      "text": "#include <leaf.h>\n",
// CHECK-NEXT: "site_b": 83,
// CHECK-NEXT: "site_e": 87,
// CHECK:      "angled": true,
// CHECK-NEXT: "directive_line_b": 83,
// CHECK-NEXT: "directive_line_e": 106,
// CHECK-NEXT: "keyword_b": 89,
// CHECK-NEXT: "keyword_e": 96,
// CHECK-NEXT: "operand_b": 97,
// CHECK-NEXT: "operand_e": 105,

// A header name produced by macro expansion has no bytes of its own in the
// directive, so no operand range is recorded for it.

// CHECK:      "site_b": 128,
// CHECK-NEXT: "site_e": 142,
// CHECK:      "angled": false,
// CHECK-NEXT: "directive_line_b": 128,
// CHECK-NEXT: "directive_line_e": 142,
// CHECK-NEXT: "keyword_b": 129,
// CHECK-NEXT: "keyword_e": 136,
// CHECK-NEXT: "spans":

//--- inc.c
#include  "leaf.h"   /* trailing comment */
#include /* spans
   lines */ "leaf.h"
# \
  include <leaf.h>
#define LEAF "leaf.h"
#include LEAF
//--- leaf.h
int leaf;
