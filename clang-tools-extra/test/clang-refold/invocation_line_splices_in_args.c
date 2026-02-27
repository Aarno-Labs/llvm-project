// RUN: %clang-refold-tester-with-lines invocation_line_splices_in_args

// test89: line splices inside invocation.
#define R(a,b,c) int r89 = (a)+(b)+(c);
R(1, 2 \
, 3)
int main(){ return r89; }
