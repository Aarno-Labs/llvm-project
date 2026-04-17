static const char *s_1 = "CAT(bill, y)"; int billy(void) { return 1; }
int main(void) {
  return billy() == 1 && s_1[0] ? 0 : 1;
}
