typedef int int_t;
int_t global114 = 2;
int make_int_t(void) { return global114; }
int main(void) {
  return make_int_t() == 2 ? 0 : 1;
}
