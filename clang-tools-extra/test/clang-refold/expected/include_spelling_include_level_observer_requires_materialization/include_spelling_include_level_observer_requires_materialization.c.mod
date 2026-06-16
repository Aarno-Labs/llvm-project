// RUN: %clang-refold-tester-clang-flags include_spelling_include_level_observer_requires_materialization -- -I headers/include_spelling_level
int p = 3;
int level = 2;
int q = 2;
int main(void) { return p + q + level; }
