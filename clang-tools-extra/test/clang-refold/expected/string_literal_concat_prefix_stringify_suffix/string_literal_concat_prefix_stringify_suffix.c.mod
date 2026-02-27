// RUN: %clang-refold-tester-with-lines string_literal_concat_prefix_stringify_suffix

// test109.c: "prefix" STR(X) "suffix"
#define STR1(x) #x
#define STR(x) STR1(x)

#define A 123
const char *s109 = "pre_" STR(124) "_suf"; // "pre_123_suf"

int main(void) {
  return (s109[4] == '1' && s109[6] == '3') ? 0 : 1;
}
