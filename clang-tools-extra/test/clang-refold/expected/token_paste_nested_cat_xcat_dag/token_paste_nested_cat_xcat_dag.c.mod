// RUN: %clang-refold-tester-with-lines token_paste_nested_cat_xcat_dag

// test69: Nested CAT/XCAT DAG-style paste.
#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)
#define MAKE_NAME(T) XCAT(array_,T)
int MAKE_NAME(float)(void) { return 1; }
int MAKE_NAME(short)(void) { return 2; }
int main() { return MAKE_NAME(float)() + MAKE_NAME(short)(); }
