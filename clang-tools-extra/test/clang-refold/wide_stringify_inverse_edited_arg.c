// RUN: %clang-refold-tester wide_stringify_inverse_edited_arg
#define W(x) L ## #x
const void *p = W(foo);
