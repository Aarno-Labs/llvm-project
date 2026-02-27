// RUN: %clang-refold-tester-with-lines if_expression_with_paste_created_identifier

// test108.c: #if with paste-created identifiers (note: 'defined' operand doesn't expand).
#define CAT(a,b) a##b
#define XCAT(a,b) CAT(a,b)

#define FLAG foo
#define ENABLED_foo 1
#define ENABLED_bar 0

#define IS_ENABLED_DIRECT(name) (XCAT(ENABLED_,name))

#if IS_ENABLED_DIRECT(FLAG)
  #define ACTIVE_MSG "enabled"
#else
  #define ACTIVE_MSG "disabled"
#endif

const char *s108 = "disabled";

int main(void) {
  return (s108[0] == 'e') ? 0 : 1;
}
