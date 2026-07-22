
extern const unsigned short int **ctype_table(void);
int skip_space(char *where, unsigned where_index_xj) {
  return ((*ctype_table())[(int) ((((unsigned char)(where[where_index_xj]))))] & (unsigned short int) 1u);
}
int keep_space(unsigned char value) {
  return ((*ctype_table())[(int) ((((unsigned char)(value))))] & (unsigned short int) 1u);
}
