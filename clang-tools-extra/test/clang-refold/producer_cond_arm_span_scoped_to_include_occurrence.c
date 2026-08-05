// RUN: clang -E -P -I %S/headers --refold-map=%t.json %s -o %t.i
// RUN: FileCheck %s --check-prefix=ARMS < %t.json
// RUN: FileCheck %s --check-prefix=SLOTS < %t.json

// A conditional arm's `pp_span`, and the arm slots derived from it, must be
// computed over the tokens of *this* include occurrence.
//
// cond_once_toggle.h disables itself, so the second inclusion takes no arm and
// contributes no preprocessed tokens.  Selecting the arm's tokens by physical
// path alone answered the second occurrence with the first occurrence's token
// run: the non-taken arm was reported selected and was handed a `pp_span`, and
// `arm_begin`/`arm_end` were handed PP indices, all belonging to the earlier
// occurrence.

#include "cond_once_toggle.h"
int between = 0;
#include "cond_once_toggle.h"

// The occurrence that really took the arm keeps its span.
// ARMS:      "file": "{{.*}}cond_once_toggle.h"
// ARMS:      "selected": true,
// ARMS-NEXT: "pp_span": {

// The occurrence that took no arm is not selected and gets no span at all;
// `}` on the next line is what proves `pp_span` was not emitted.
// ARMS:      "file": "{{.*}}cond_once_toggle.h"
// ARMS:      "selected": false
// ARMS-NEXT: }

// Same split for the slots seeded from that span.
// SLOTS:      "kind": "arm_begin",
// SLOTS:      "ref": 0,
// SLOTS-NEXT: "owner_include_id": {{[0-9]+}},
// SLOTS-NEXT: "pp": 0

// SLOTS:      "kind": "arm_begin",
// SLOTS:      "ref": 1,
// SLOTS-NEXT: "owner_include_id": {{[0-9]+}}
// SLOTS-NEXT: }
