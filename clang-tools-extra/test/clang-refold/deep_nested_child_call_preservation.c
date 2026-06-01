// RUN: %clang-refold-tester-with-lines deep_nested_child_call_preservation
#define ID1(x) x
#define ID2(x) ID1(x)
#define ID3(x) ID2(x)
#define ID4(x) ID3(x)
#define ID5(x) ID4(x)
#define ID6(x) ID5(x)
#define ID7(x) ID6(x)
#define ID8(x) ID7(x)
#define ID9(x) ID8(x)
#define ID10(x) ID9(x)
#define PAIR_EXPR(a, b) a + b
#define MAKE_ARRAY(a, b) int arr[] = a, b;
#define WRAP_ARRAY(a, b) MAKE_ARRAY(a, b)
WRAP_ARRAY({PAIR_EXPR(ID10(ID9(ID8(ID7(ID6(ID5(ID4(ID3(ID2(ID1(1)))))))))), 2), 3})
