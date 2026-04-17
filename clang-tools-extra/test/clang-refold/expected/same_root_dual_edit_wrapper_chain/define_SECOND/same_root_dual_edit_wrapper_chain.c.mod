// RUN: %clang-refold-tester same_root_dual_edit_wrapper_chain FIRST
// RUN: %clang-refold-tester same_root_dual_edit_wrapper_chain SECOND
// RUN: %clang-refold-tester same_root_dual_edit_wrapper_chain THIRD
// RUN: %clang-refold-tester same_root_dual_edit_wrapper_chain AMBIGUOUS
// Test: same_root_dual_edit_wrapper_chain
// Refold intent: strengthen coverage for same-root multi-edit wrapper/stringify/paste rewrites under one preserved root

#define CAT(a, b) a##b
#define XCAT(a, b) CAT(a, b)
#define STR(x) #x
#define XSTR(x) STR(x)
#define P3(a, b, c) XCAT(XCAT(a, b), c)

static const char *s = XSTR(P3(pre_, mid_long_, sufx));
int main(void) {
  return s[0] ? 0 : 1;
}
