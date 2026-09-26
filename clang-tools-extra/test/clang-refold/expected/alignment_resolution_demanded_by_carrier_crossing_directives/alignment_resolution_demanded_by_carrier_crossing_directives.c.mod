// RUN: %clang-refold-tester alignment_resolution_demanded_by_carrier_crossing_directives
// RUN: FileCheck --input-file=%t/outputs/alignment_resolution_demanded_by_carrier_crossing_directives.out %s
//
// Regression: a direct-TU carrier rejected for crossing protected structure
// must name the hunk it was planned from, so that the run can ask whether a
// different alignment avoids the crossing.
//
// The edit deletes `x`.  The `;` before it repeats its own `;`, and the
// `static int` after it repeats its own first two tokens, so the deletion has
// four optimal alignments.  The first attempt plans on core-forced anchors
// alone, which leave one replacement hunk spanning both `#define` lines.  The
// global source-edit audit rejects that carrier.  Its terminal request used to
// carry no token range: alignment resolution was never demanded, no ladder
// step could act on it, and the run failed with "no admissible refold".
//
// Now the request names the hunk and resolution is probed.  The expected
// output deletes exactly `static int x;` between the two directives.  Two
// repairs reach it once the request is attributed -- the structure-respecting
// repair commits it during resolution, and without that rule the line-aligned
// hunk narrowing re-anchors the same map -- so the CHECK lines assert the
// attribution and the probe, which neither repair can supply.
//
// CHECK: terminal fallback requested: action=raw-b-emission obligation=EmissionEditSetComposable reason=UncomposableEmissionEditSet context=hunk=0,aTokens=[2,9),bTokens=[2,5)
// CHECK: attempt 0 is limited by alignment ambiguity, and 1 of 1 certified window(s) carry it
int y;
#define A

#define B
static int z;
