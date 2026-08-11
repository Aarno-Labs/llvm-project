struct S { int *storage; };
int probe(struct S *ot, int j)
{
  if (((ot->storage[j]) >= 0))
    return 1;
  return 0;
}
