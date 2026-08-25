// RUN: %clang-refold-tester directive_seam_insertion_blank_line_before_conditional

// Control for directive_seam_insertion_before_indented_conditional: a blank
// line separates the `#endif` from the following `#if`, so the insertion
// anchors on an ordinary source byte rather than on a protected directive
// beginning.  This case never reached the zero-width-insertion rule; the test
// guards against a fix there that over-corrects and changes where an
// unconstrained insertion is placed.
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
