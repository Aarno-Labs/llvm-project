// RUN: clang -E -P -I %S/headers --refold-map=%t.json %s -o %t.i
// RUN: FileCheck %s --check-prefix=MAP < %t.json

// A conditional arm's recorded condition and body start must span the whole
// *logical* directive.  Translation phase 2 splices a physical line whose
// newline is preceded by a backslash onto the next one, so ending the directive
// at the first newline puts the arm body start inside the directive and drops
// the continuation from the condition text -- which the consumer scans for
// builtins and macro state.

#define RF_SPLICE_B 1
#include "cond_spliced_directive.h"
int use = cond_spliced_taken;

// The condition carries both operands, so the continuation line was consumed.
// MAP: "cond": "{{.*}}RF_SPLICE_A{{.*}}RF_SPLICE_B{{.*}}"
