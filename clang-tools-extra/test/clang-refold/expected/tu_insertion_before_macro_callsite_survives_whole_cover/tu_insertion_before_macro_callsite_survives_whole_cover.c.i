int cmp(const char *d, const int *s)
{
  if (*d != ((*s) + 1))
    return 1;
  return 0;
}
