// RUN: %clang-refold-tester macro_include_filename_via_paste_stringify
// Test: macro_include_filename_via_paste_stringify
// Refold intent: preserve include filenames built through paste and stringify, not just literal include macros

#include "common.h"
#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a##b
#define HNAME(n) CAT(gap_h, n).h
#define HDR(n) XSTR(HNAME(n))

// NOTE: This test effecitively modifies the contents of this include so that
// the entire contents matches another include such that this could be refolded
// as the following:
//   #include HDR(2)
// But I'm not sure we should consider this class of refolding since I don't
// really think this would come up that often (if at all).
#include HDR(1)
RF_MARK(after_gap_include)

int main(void) {
  return gap_value == 11 ? 0 : 1;
}
