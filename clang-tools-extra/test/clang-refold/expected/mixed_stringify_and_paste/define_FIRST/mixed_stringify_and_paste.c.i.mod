static const char *s_1 = "CAT(bill, z)"; int billz(void) { return 1; }
int main(void) {
  return billz() == 1 && s_1[0] ? 0 : 1;
}
