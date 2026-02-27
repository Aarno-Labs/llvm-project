// RUN: %clang-refold-tester-with-lines invocation_nested_braces_brackets_parens_in_args

// test91: nested braces/brackets/parens.
#define R3(x) int n91 = (x);
R3(((int[]){1,2,3})[2])
int main(){ return n91; }
