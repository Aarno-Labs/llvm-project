// RUN: %clang-refold-tester-with-lines macro_arg_rewrite_simple

// test62: Simple function-like macro arg rewrite.
#define ID(T) T
ID(float) test62_fn(ID(int) x) { return x; }
int main() { return (int)test62_fn(3); }
