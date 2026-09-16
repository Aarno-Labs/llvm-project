struct slot {
  int index[4];
};
struct table {
  struct slot *storage;
};
int probe(struct table *ot, struct slot *ob, int ob_index, int j) {
  if (((ob->index[j]) >= 0)) {
    return 1;
  }
  return 0;
}
