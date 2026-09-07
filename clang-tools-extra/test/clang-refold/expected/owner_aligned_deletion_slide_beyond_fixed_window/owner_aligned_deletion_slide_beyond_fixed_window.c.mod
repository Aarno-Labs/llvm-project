// RUN: %clang-refold-tester owner_aligned_deletion_slide_beyond_fixed_window
// The owner-closing slide for a straddling deletion run may sit further than
// any fixed search window, and refusing it loses a refold the streams prove.
//
// This is `deleted_include_preserves_pushed_macro_state` with a longer
// repetition.  As there, deleting every token the header contributed must
// still keep its `#pragma push_macro`, `#undef` and `#define`: the
// translation unit pops the macro after the include has ended, so the header
// has to materialize rather than simply disappear.
//
// B deletes 72 tokens -- as many as the header contributed.  The translation
// unit repeats the header's first 66 literals right after the include, so
// those 72 deleted tokens can equally be taken 66 positions further right and
// still realize B.  The owner-depth tie-break is an additive
// per-deleted-token cost, so it takes exactly that: deleting the header's
// last 6 tokens and all 66 translation-unit ones costs 6, while deleting the
// header's own 72 costs 72.  No single owner covers the run it picks,
// `OwnerClosedCover` fails, and the attempt asks for the terminal carrier.
//
// Moving the run back onto the header's cover takes 66 steps, one per
// repeated literal.  Every step exchanges two identically spelled tokens, so
// the alignment keeps its exact matched-token count and the repair is proven
// by the streams themselves -- yet a slide search stopping at a fixed
// 64-token window rejects it two steps short and gives the region up instead.
#define SLIDE_STATE 1
const char slide_join[] =
#pragma push_macro("SLIDE_STATE")
#undef SLIDE_STATE
#define SLIDE_STATE 2
// Seventy-two distinct string-literal tokens spliced into the middle of the
// translation unit's initializer.  Adjacent literals concatenate, so this is
// an ordinary generated-data-table include.  Nothing here recurs: the only
// repetition in the stream is the copy of the first 66 that the translation
// unit spells out after the include.

"r00" "r01" "r02" "r03" "r04" "r05"
"r06" "r07" "r08" "r09" "r10" "r11"
"r12" "r13" "r14" "r15" "r16" "r17"
"r18" "r19" "r20" "r21" "r22" "r23"
"r24" "r25" "r26" "r27" "r28" "r29"
"r30" "r31" "r32" "r33" "r34" "r35"
"r36" "r37" "r38" "r39" "r40" "r41"
"r42" "r43" "r44" "r45" "r46" "r47"
"r48" "r49" "r50" "r51" "r52" "r53"
"r54" "r55" "r56" "r57" "r58" "r59"
"r60" "r61" "r62" "r63" "r64" "r65"
  ;
int slide_inner = SLIDE_STATE;
#pragma pop_macro("SLIDE_STATE")
int slide_outer = SLIDE_STATE;
