static const char *s = "alpha" ":" "bravo";
int main(void) {
  return s[0] ? 0 : 1;
}
