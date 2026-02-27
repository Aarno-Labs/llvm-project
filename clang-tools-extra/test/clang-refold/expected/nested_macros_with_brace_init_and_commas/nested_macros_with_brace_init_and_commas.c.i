int main(void) {
  int *p = (int[2]){ ((3)), ((4)) };
  return ((p)[0] + (p)[1]) == 7 ? 0 : 1;
}
