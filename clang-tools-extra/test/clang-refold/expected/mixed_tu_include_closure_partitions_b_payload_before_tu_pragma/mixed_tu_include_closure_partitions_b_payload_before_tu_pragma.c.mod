// RUN: %clang-refold-tester mixed_tu_include_closure_partitions_b_payload_before_tu_pragma
// B-payload/preserved-source partition regression, opposite side: the surviving
// B material of the closure comes from the include run spelled *before* the
// preserved `#pragma`, so the payload is cut at the end of the material and the
// directive is emitted after it.  This pins that the split is derived from the
// pragma's A frontier rather than assumed to be at one edge.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = {
2,
#pragma GCC poison FOO
 9
};
