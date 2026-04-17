static const char *s_1 = "CAT(jill, y)"; int jilly(void) { return 1; }
int main(void) {
  return jilly() == 1 && s_1[0] ? 0 : 1;
}
