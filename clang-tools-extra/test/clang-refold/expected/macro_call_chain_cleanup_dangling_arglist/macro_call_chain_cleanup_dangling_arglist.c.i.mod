typedef int long_t;
long_t global114 = 2;
int make_long_t(void) { return global114; }
int main(void) {
  return make_long_t() == 2 ? 0 : 1;
}
