// A TU byte-span edit is refused when its bytes lie inside a macro callsite
// that no proven macro rewrite accounts for.
//
// B renames only `int x`.  `x` is CALL's actual `a`, which G also pastes into
// the unchanged `g_x`, so the args-only rewrite is refused, and whole cover is
// refused because G's `)` closes after the `_Pragma` in the consumed source.
// The tokmap maps the edited `x` to its spelling in the actual, and the TU
// byte-span path used to accept rewriting those bytes to `y`, which also
// turns `g_x` into `g_y`.  Here a later composition failure happens to hide
// that edit; with a `#define` in place of the `_Pragma`, which is not valid C,
// it was the output.
// The fold itself is not asserted: keeping the `_Pragma` inside a whole-cover
// replacement is a sound refold that is not implemented yet.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir && cp %s %t.dir/t.c
// RUN: cd %t.dir && clang -E -P --refold-map=map.json t.c -o a.i
// RUN: sed -e 's/int x;/int y;/' a.i > b.i
// RUN: clang-refold --strict --verify-output=off --log-level=trace --pp a.i \
// RUN:   --pp-mod b.i --refold-map map.json --out out.c > log.txt 2>&1 || true
// RUN: grep 'reject inv id=[0-9]* name=CALL: unchanged A token 4' log.txt
// RUN: not grep 'accept owner realization evidence=TUByteSpan' log.txt
#define G(v) g_##v
#define CALL(f, a) int a; int f(a
CALL(G, x) _Pragma("refold_marker") );
