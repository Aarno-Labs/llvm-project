// RUN: %clang-refold-tester directive_seam_insertion_before_unindented_conditional

// Control for directive_seam_insertion_before_indented_conditional: the same
// `#endif`/`#if` seam, differing only in that the line after the second
// `#endif` starts at column zero, so the insertion payload ends exactly at its
// final newline.  Both must refold, and the relaxed logical-line-beginning
// test must not disturb this already-admitted case.
int seam_probe(int argc) {
    int total = argc;
#if 1
    total += 1;
#endif
#if 0
    total += 2;
#endif
total += 3;
    return total;
}
