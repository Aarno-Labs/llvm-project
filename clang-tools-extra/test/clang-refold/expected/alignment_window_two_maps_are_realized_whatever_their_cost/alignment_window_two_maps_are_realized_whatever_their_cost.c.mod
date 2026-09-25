// RUN: env CLANG_REFOLD_TEST_ONLY_SEMANTIC_REALIZATION_COST_BUDGET=0 %clang-refold-tester alignment_window_two_maps_are_realized_whatever_their_cost
// RUN: FileCheck --input-file=%t/outputs/alignment_window_two_maps_are_realized_whatever_their_cost.out %s
//
// A two-map window is always decided, whatever realizing it costs.
//
// The edit deletes `tag_name`, whose closing `;` repeats the `;` that ends the
// included header, so the deletion has two optimal alignments.  Map 1 deletes
// the header's `;` and keeps this file's; realizing it needs the include
// closure, which refuses to absorb the MIN_FREE invocation that
// MIN_CRITICAL_FREE's replacement list plants in the gap, so map 1 requests
// terminal fallback.  Map 2 deletes exactly the definition's own bytes.
//
// The budget is injected at zero so that this small input reaches the decline
// that a translation unit above 65536 A tokens reaches in production
// (`mquickjs.c`).  Before the floor, the least-source-mutation rule declined
// after realizing map 1, the window kept only core-forced anchors, and the hunk
// those leave spans both `;` tokens: it has no owner, so the refold failed with
// "no admissible refold".  With the floor, map 2 is realized, is the unique
// accepted map, and is committed; the expected output keeps the comment, every
// `#define`, and the `#include`.
//
// CHECK: window 0 carries alignment ambiguity over A=[0,68) B=[0,44): 2 distinct core-optimal map(s) enumerated
// CHECK: candidate map 1 of 2 realized: requested terminal fallback
// CHECK: candidate map 2 of 2 realized: accepted
// CHECK: window 0 committing globally least source-mutation class: accepted=1 least=1 classMembers=[1]
#include "two_map_floor_tail.h"

/* a comment between the include and the definition */
#define MAYBE_UNUSED __attribute__((unused))
#define MIN_FREE 512
#define MIN_CRITICAL_FREE (MIN_FREE - 256)
#define TAG_COUNT 3



typedef struct {
  int x;
} after_t;

int use(after_t *p) { return p->x + MIN_CRITICAL_FREE; }
