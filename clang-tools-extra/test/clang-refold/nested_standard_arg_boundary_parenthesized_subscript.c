// RUN: %clang-refold-tester nested_standard_arg_boundary_parenthesized_subscript
extern const unsigned short int **ctype_table(void);

#define DIGIT_MASK 2u
#define CAST(type, value) ((type)(value))
#define IS_DIGIT(value) \
  ((*ctype_table())[(int) ((value))] & (unsigned short int) DIGIT_MASK)

struct holder {
  struct {
    char *s;
  } value;
};

int read_next(struct holder *m, char *s, unsigned s_index_xj) {
  return IS_DIGIT(CAST(unsigned char, *++s));
}

int keep_next(char *s) {
  return IS_DIGIT(CAST(unsigned char, *s));
}
