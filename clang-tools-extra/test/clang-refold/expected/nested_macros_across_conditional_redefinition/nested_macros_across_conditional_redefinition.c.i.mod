int mode_a(void) { return 13; }
int mode_b(void) { return 17; }
int main(void) {
  return mode_b() == 13 ? 0 : 1;
}
