void __assert_fail(const char *);
struct T { int len; };
struct G { struct T tbl[63]; };
struct T tbl[63];
void f(struct G *xjg, const struct T *p) {
  int p_index_xj = 0;
  ((void) sizeof ((p - tbl == 62) ? 1 : 0), ({ if (p - tbl == 62) ; else __assert_fail("p - tbl == N"); }));
  ((void) sizeof ((p - tbl == 62) ? 1 : 0), ({ if (p - tbl == 62) ; else __assert_fail("p - tbl == N"); }));
  ((void) sizeof ((p - tbl == 62) ? 1 : 0), ({ if (p - tbl == 62) ; else __assert_fail("p - tbl == N"); }));
  ((void) sizeof ((p - tbl == 62) ? 1 : 0), ({ if (p - tbl == 62) ; else __assert_fail("p - tbl == N"); }));
  ((void) sizeof ((p - tbl == 62) ? 1 : 0), ({ if (p - tbl == 62) ; else __assert_fail("p - tbl == N"); }));
  ((void) sizeof ((p - tbl == 62) ? 1 : 0), ({ if (p - tbl == 62) ; else __assert_fail("p - tbl == N"); }));
}
