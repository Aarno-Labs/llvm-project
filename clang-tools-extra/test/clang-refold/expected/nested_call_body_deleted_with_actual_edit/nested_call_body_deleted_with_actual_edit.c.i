const char *pick(int k) {
  const char *fmt = "";
  switch (k) {
    case 0: fmt = ("zero"); break;
    case 1: fmt = ("one"); break;
  }
  return fmt;
}
