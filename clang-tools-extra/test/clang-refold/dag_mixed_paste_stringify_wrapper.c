// RUN: %clang-refold-tester dag_mixed_paste_stringify_wrapper
#define S(x) #x
#define CAT(a,b) a##b
#define W(p,x) CAT(p, _tag) S(x)
const char* r = W(foo, bar);
