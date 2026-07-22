
extern const unsigned short int **ctype_table(void);
struct holder {
  struct {
    char *s;
  } value;
};
int read_next(struct holder *m, char *s, unsigned s_index_xj) {
  return ((*ctype_table())[(int) ((((unsigned char)((m->value.s)[++s_index_xj]))))] & (unsigned short int) 2u);
}
int keep_next(char *s) {
  return ((*ctype_table())[(int) ((((unsigned char)(*s))))] & (unsigned short int) 2u);
}
