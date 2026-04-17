static const char *s_1 = "CAT(gill, z)"; int gillz(void) { return 1; }
int main(void) {
  return gillz() == 1 && s_1[0] ? 0 : 1;
}
