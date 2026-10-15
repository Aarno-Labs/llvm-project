typedef int (*fn)(int);
int id(int v) { return v; }
fn tab[2] = {id, id};
fn tbl[2] = {id, id};
#define AT(i) tab[i]
int r(void) { return AT(0)(3); }
