// RUN: %clang-refold-tester structural_gap_straddle_composed_define_and_consumed_pragma_commits_payload
// A gap holding two structures of different kinds is crossed by proving each
// one separately, and commits the payload when every one of them is proven.
//
// This file was written to pin composition on the premise that a `#pragma` was
// simply "one directive of any other kind".  It was not, and the premise was
// wrong twice over: `region` was unclassified, and the crossing gated on the
// state effect as a stand-in for whether the preprocessor emits anything.  The
// gap would have refused with nothing else in it, so the file pinned the
// negative half instead -- one unanswerable structure taking the whole gap down.
//
// Both halves are now answered from facts rather than from the effect enum, so
// the gap is crossable and the trace carries a proof per structure:
//
//   structure=MacroDefine ... crossable=true proof=MacroStateDirectiveUnobserved
//   structure=Pragma      ... crossable=true proof=ConsumedPragmaUnobserved
//
// Both directives survive into the refolded source; composition is what lets
// the payload past them rather than around them.
//
// The negative half it used to hold has moved to
// `structural_gap_straddle_unclassified_consumed_pragma_refuses`, whose pragma
// is one no classification covers, so the property stays pinned by an input
// this package did not make answerable.
int arr[] = { 
#define GAP_A 1
#pragma region
9 };
