static int first(int d) {
  return d - 1;
}
static int second(int d) {
  return d;
}

int use(int d) { return first(d) + second(d); }
