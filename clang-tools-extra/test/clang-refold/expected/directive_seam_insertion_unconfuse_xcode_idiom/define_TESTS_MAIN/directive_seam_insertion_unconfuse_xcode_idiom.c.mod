// RUN: %clang-refold-tester directive_seam_insertion_unconfuse_xcode_idiom TESTS_MAIN

// The widespread "unconfuse Xcode" idiom, in which a conditional selects one
// of two function headers and an immediately following `#if 0` block carries
// the balancing brace.  With the conditional taken, the first preprocessed
// byte of the function body maps back to the first byte of the `#if 0`
// directive, which the closing `#endif` of the header conditional abuts.  An
// insertion at the top of the body therefore lands exactly on the
// `#endif`/`#if` seam with an indented line following it.
#ifdef TESTS_MAIN
int seam_main(int argc) {
#else
int seam_tests_main(int argc);
int seam_tests_main(int argc) {
#endif
    int seam_insert = 0;
    #if 0 /* unconfuse xcode */
}
#endif
    int total = argc;
    total += 3;
    return total;
}
