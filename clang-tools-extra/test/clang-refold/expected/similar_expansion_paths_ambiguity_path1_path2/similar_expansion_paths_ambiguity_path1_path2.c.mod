// RUN: %clang-refold-tester-with-lines similar_expansion_paths_ambiguity_path1_path2

// test116.c: two different macro paths that can yield similar token streams.
#define ID(x) x
#define WRAP(x) ID(x)

#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)

#define A foo
#define B bar

int foobar(void) { return 40; }

#define PATH1() XCAT(A,B)
#define PATH2() WRAP(XCAT(A,B))

int main(void) {
  return PATH1()() == 40 && foobaz() == 40 ? 0 : 1;
}
