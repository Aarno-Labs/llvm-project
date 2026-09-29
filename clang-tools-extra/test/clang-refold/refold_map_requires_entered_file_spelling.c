// An include record with a parent must carry entered_file_spelling; a map
// that lacks it is malformed and is rejected.
//
// A child include keeps its directive after its parent is materialized only
// when the relocated directive re-enters the child with the spelling that
// preserved __FILE__ and __FILE_NAME__ observers saw.  That spelling is proved
// against entered_file_spelling alone.  The consumer used to accept a map
// without it and fall back on other witnesses: the decoded __FILE__ payload,
// a replay of the original directive, or resolved_path.  The producer records
// the spelling of every include Clang enters, and only an entered include gets
// a parent, so none of those fallbacks can be reached from the producer, and
// there is no longer one to take.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: clang -E -P --refold-map=%t.dir/map.json %s -o %t.dir/a.i
// RUN: sed -e 's/int p = 1;/int p = 10;/' %t.dir/a.i > %t.dir/b.i
//
// The unmodified map refolds.
// RUN: clang-refold --strict --pp %t.dir/a.i --pp-mod %t.dir/b.i \
// RUN:   --refold-map %t.dir/map.json --out %t.dir/ok.c
//
// RUN: python3 -c "import json, sys; m = json.load(open(sys.argv[1])); \
// RUN:   c = [i for i in m['items'] if i.get('subkind') == '#include' and 'parent' in i]; \
// RUN:   assert len(c) == 1 and c[0]['target'] == '\"child.h\"'; \
// RUN:   c[0].pop('entered_file_spelling'); c[0].pop('entered_file_name'); \
// RUN:   json.dump(m, open(sys.argv[2], 'w'))" %t.dir/map.json %t.dir/missing.json
// RUN: not clang-refold --strict --pp %t.dir/a.i --pp-mod %t.dir/b.i \
// RUN:   --refold-map %t.dir/missing.json --out %t.dir/missing.c 2>&1 \
// RUN:   | FileCheck %s
// CHECK: failed to parse refold '{{.*}}missing.json' JSON file: oneOf failed at $.items[{{[0-9]+}}]
#include "headers/refold_map_requires_entered_file_spelling/parent.h"
int main(void) { return p + q + (v[0] != 0); }
