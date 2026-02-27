// RUN: %clang-refold-tester-with-lines macro_arg_rewrite_second_invocation_only

// test63: Two invocations; edit only the second invocation.
#define ID(T) T
ID(int) f1(ID(int) x) { return x + 1; }
ID(int) f2(ID(int) x) { return x + 2; }
int main() { return f1(1) + f2(2); }
