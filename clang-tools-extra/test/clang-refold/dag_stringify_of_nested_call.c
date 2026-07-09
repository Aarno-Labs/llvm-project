// RUN: %clang-refold-tester dag_stringify_of_nested_call
#define S(x) #x
#define F(a,b) a+b
const char* s = S(F(1,2));
