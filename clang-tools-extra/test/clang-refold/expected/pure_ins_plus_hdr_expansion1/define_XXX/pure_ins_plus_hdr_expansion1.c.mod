// RUN: %clang-refold-tester pure_ins_plus_hdr_expansion1 XXX
int some_prefix_func(char* c);


#ifdef XXX
int first(short x);
#define FOO(X) (X + X)
int last(int x);
#else
float first(float x);
#define FOO(X) X
float last(float x);
#endif

int main() {
  return FOO(5);
}
