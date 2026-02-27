// RUN: %clang-refold-tester-with-lines token_paste_pre_x_post_edit_middle_segment

// test71: pre_##X##_post; edit X segment only.
#define MAKE(X) int pre_##X##_post = 1;
MAKE(foo)
int main() { return pre_foo_post; }
