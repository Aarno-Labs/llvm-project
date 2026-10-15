typedef int (*fn)(int);
int id(int v) { return v; }
fn tab[2] = {id, id};
fn tbl[2] = {id, id};
int r(void) { return tbl[0](3); }
int w = 3;
