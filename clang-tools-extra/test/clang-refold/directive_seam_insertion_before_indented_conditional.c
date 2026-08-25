// RUN: %clang-refold-tester directive_seam_insertion_before_indented_conditional

// A zero-width insertion whose original-source anchor is the first byte of a
// `#if` directive line that is itself immediately preceded by `#endif`, so no
// ordinary source byte separates the two protected intervals and the carrier
// has nowhere else to anchor.  Token-aligned diffing gives the insertion hunk
// the horizontal white-space run preceding the next surviving token, so the
// payload ends in "\n    " rather than "\n".  C allows white-space before `#`,
// the directive introducer still begins its logical line, and the carrier must
// be admitted instead of driving the whole translation unit to a terminal
// fallback.  Companion controls:
// directive_seam_insertion_before_unindented_conditional (same shape, payload
// ends exactly at the newline) and
// directive_seam_insertion_blank_line_before_conditional (an ordinary byte
// separates the two directives).
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
