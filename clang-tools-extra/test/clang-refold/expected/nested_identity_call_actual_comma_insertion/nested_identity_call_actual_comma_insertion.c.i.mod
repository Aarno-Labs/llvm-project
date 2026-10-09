const char *pick(int k) {
  const char *fmt = "";
  int idx = -1;
  switch (k) {
    case 0: fmt = "zero", idx = 0; break;
    case 1: fmt = "one"; break;
  }
  return idx < 0 ? fmt : "";
}
