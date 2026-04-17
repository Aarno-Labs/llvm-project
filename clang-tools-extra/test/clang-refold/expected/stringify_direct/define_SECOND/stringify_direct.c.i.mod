static const char *s = "alpha" "," "bravo_more";
int main(void) {
  return s[0] ? 0 : 1;
}
