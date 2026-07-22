// RUN: %clang-refold-tester standard_arg_boundary_subscript_suffix
extern const unsigned short int **ctype_table(void);

#define SPACE_MASK 1u
#define CAST(type, value) ((type)(value))
#define IS_SPACE(value) \
  ((*ctype_table())[(int) ((value))] & (unsigned short int) SPACE_MASK)

int skip_space(char *where, unsigned where_index_xj) {
  return IS_SPACE(CAST(unsigned char, where[where_index_xj]));
}

int keep_space(unsigned char value) {
  return IS_SPACE(CAST(unsigned char, value));
}
