// RUN: %clang-refold-tester-verify-off structural_tiling_consumes_function_like_callsite_across_pragma
// RUN: FileCheck --input-file=%t/outputs/structural_tiling_consumes_function_like_callsite_across_pragma.out %s
//
// Regression: a replacement straddling a preserved `#pragma` whose first
// physical run begins with a function-like invocation.  `ID(1)` expands to its
// argument alone, so its only A token maps to the `1` inside the parentheses,
// and no token spelling equals the recorded callsite extent.
//
// The tiler's physical run plan therefore saw `ID(` as a gap it could not prove
// and abandoned the hunk, leaving only the raw edited stream.  The producer's
// root invocation record proves the whole cover lies inside the callsite, so
// the run consumes the callsite spelling once and the pragma is preserved.
//
// The TU segment the tiling certifies must name the same source bytes as the
// edit that realizes it.  A min/max over token spellings starts that segment at
// the `1` and certifies a source cover that silently omits `ID(`.
//
// CHECK: structural tiling accepted:
// CHECK: segment 0 A=[{{[0-9]+}},{{[0-9]+}}) B=[{{[0-9]+}},{{[0-9]+}}) source='structural_tiling_consumes_function_like_callsite_across_pragma.c'{{\[}}[[SEGB:[0-9]+]],[[SEGE:[0-9]+]]) owner=kind=TU
// CHECK: proof uniquePartition=true sourceByteCoverComplete=true
// CHECK: accept owner realization evidence=TUByteSpan owner=TU source='structural_tiling_consumes_function_like_callsite_across_pragma.c'{{\[}}[[SEGB]],[[SEGE]])
#define ID(x) x
int arr[] = { ID(1),
#pragma region
2 };
