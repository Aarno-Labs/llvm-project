int main(void) {
  int *p = (int[2]){ ((3)), ((5)) };
  return ((p)[0] + (p)[1]) == 8 ? 0 : 1;
}
