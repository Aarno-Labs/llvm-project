// RUN: %clang-refold-tester-with-lines paste_child_rewrite_rejected_by_parent_formal_uses
// Refold intent: preserve parent use of a child paste-created argument when structurally certifiable

#define XCAT(a, b) a##b
#define USE(x) int x(void) { return 1; } int call_##x(void) { return x(); }
USE(XCAT(pre_, one))
int main(void) {
  return 0;
}
