// A rewrite whose re-expansion matches B is not refuted because the token
// diff placed part of its output past the invocation's cover.
//
// B replaces `*++s` with `(m->value.s)[++s_index_xj]`.  The diff puts one of
// the new `)` after a run of identical `)` that ends CAST's cover, outside
// it.  The check that a slot's patch reproduces B maps the cover to B across
// such a run, so `CAST(unsigned char, (m->value.s)[++s_index_xj])` is kept.
// Mapping only the cover's own edges refuted it, although its re-expansion is
// exactly B; another candidate happened to produce the same fold here.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir && cp %s %t.dir/t.c
// RUN: cd %t.dir && clang -E -P --refold-map=map.json t.c -o a.i
// RUN: sed -e 's/((unsigned char)(\*++s))/((unsigned char)((m->value.s)[++s_index_xj]))/' a.i > b.i
// RUN: clang-refold --strict --verify-output=off --log-level=trace --pp a.i \
// RUN:   --pp-mod b.i --refold-map map.json --out out.c > log.txt 2>&1
// RUN: not grep 'rewrite-realization.*refute inv id=[0-9]* name=CAST' log.txt
// RUN: FileCheck --input-file=out.c %s
// CHECK: {{^}}  return IS_DIGIT(CAST(unsigned char, (m->value.s)[++s_index_xj]));
extern const unsigned short int **ctype_table(void);

#define DIGIT_MASK 2u
#define CAST(type, value) ((type)(value))
#define IS_DIGIT(value) \
  ((*ctype_table())[(int) ((value))] & (unsigned short int) DIGIT_MASK)

struct holder {
  struct {
    char *s;
  } value;
};

int read_next(struct holder *m, char *s, unsigned s_index_xj) {
  return IS_DIGIT(CAST(unsigned char, *++s));
}

int keep_next(char *s) {
  return IS_DIGIT(CAST(unsigned char, *s));
}
