static int first_x(int d) {
  return d - 1;
}
static int second_x(int d) {
  return d;
}

int use(int d) { return first_x(d) + second_x(d); }
