// RUN: %clang-refold-tester-with-lines mixed_tu_include_closure_partitions_b_payload_around_tu_pragma_with_lines
// Line-control regression for the B-payload/preserved-source partition: the
// preserved `#pragma` line is copied out of the replaced source span, so the
// replacement keeps the original physical line count and the untouched suffix
// needs no resync.
#define KEEP(x) ((x) + 1)

int untouched = KEEP(5);

int arr[] = { 
#pragma GCC poison FOO
7
};

int after = __LINE__;
