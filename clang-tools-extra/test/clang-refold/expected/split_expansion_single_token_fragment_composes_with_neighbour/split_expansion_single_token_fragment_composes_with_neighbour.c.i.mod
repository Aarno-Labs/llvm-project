char *f(char *cursor, char *separator, int i) {
  cursor = ((!cursor || i < 0) ? ((void *)0) : (void *)0) ? cursor : separator + 1;
  return cursor;
}
