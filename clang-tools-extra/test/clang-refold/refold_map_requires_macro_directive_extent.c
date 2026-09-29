// A #define or #undef record must carry its producer extent, and the extent
// must contain the record's own name-anchored site; a map that lacks one or
// contradicts it is malformed and is rejected.
//
// The consumer used to fall back on recovering the extent from the directive's
// rendered text, which equals the source only for a canonically spelled
// definition, and to drop an extent that contradicted the site.  Clang's lexer
// now measures every macro-state directive, so neither case can come from the
// producer, and there is no longer a fallback to take.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: clang -E -P --refold-map=%t.dir/map.json %s -o %t.dir/a.i
// RUN: sed -e 's/{ 1, 2 }/{ 1, 2, 3 }/' %t.dir/a.i > %t.dir/b.i
//
// The unmodified map refolds.
// RUN: clang-refold --strict --pp %t.dir/a.i --pp-mod %t.dir/b.i \
// RUN:   --refold-map %t.dir/map.json --out %t.dir/ok.c
//
// RUN: python3 -c "import json, sys; m = json.load(open(sys.argv[1])); \
// RUN:   d = next(i for i in m['items'] if i.get('subkind') == '#define' and i['name'] == 'KEPT'); \
// RUN:   [(t.pop('site_b'), t.pop('site_e')) for t in d['replacement_tokens']]; \
// RUN:   (d.pop('directive_line_b'), d.pop('directive_line_e')) if sys.argv[2] == 'missing' \
// RUN:     else d.update(directive_line_e=d['site_e'] - 1); \
// RUN:   json.dump(m, open(sys.argv[3], 'w'))" %t.dir/map.json missing %t.dir/missing.json
// RUN: not clang-refold --strict --pp %t.dir/a.i --pp-mod %t.dir/b.i \
// RUN:   --refold-map %t.dir/missing.json --out %t.dir/missing.c 2>&1 \
// RUN:   | FileCheck --check-prefix=MISSING %s
// MISSING: failed to parse refold '{{.*}}missing.json' JSON file: oneOf failed at $.items[{{[0-9]+}}]
//
// RUN: python3 -c "import json, sys; m = json.load(open(sys.argv[1])); \
// RUN:   d = next(i for i in m['items'] if i.get('subkind') == '#define' and i['name'] == 'KEPT'); \
// RUN:   [(t.pop('site_b'), t.pop('site_e')) for t in d['replacement_tokens']]; \
// RUN:   (d.pop('directive_line_b'), d.pop('directive_line_e')) if sys.argv[2] == 'missing' \
// RUN:     else d.update(directive_line_e=d['site_e'] - 1); \
// RUN:   json.dump(m, open(sys.argv[3], 'w'))" %t.dir/map.json short %t.dir/short.json
// RUN: not clang-refold --strict --pp %t.dir/a.i --pp-mod %t.dir/b.i \
// RUN:   --refold-map %t.dir/short.json --out %t.dir/short.c 2>&1 \
// RUN:   | FileCheck --check-prefix=SHORT %s
// SHORT: Directive extent [{{[0-9]+}},{{[0-9]+}}) does not contain site [{{[0-9]+}},{{[0-9]+}})
#define KEPT 3
int before = KEPT;
int arr[] = { 1, 2 };
