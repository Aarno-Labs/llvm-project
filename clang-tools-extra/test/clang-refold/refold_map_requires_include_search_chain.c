// pp_ctx.include_search_chain is required.  An empty chain is a producer fact
// and refolds; a map without the chain is malformed and is rejected.
//
// Include replay searches exactly the directories in the chain, which the
// producer copies from Clang's HeaderSearch.  The consumer used to rebuild the
// list from argv (-I, -iquote, -isystem, -idirafter) whenever the chain was
// absent or empty.  An empty chain means Clang dropped every such directory
// because it did not exist, so the rebuilt list could only name directories
// Clang never searched.  With that fallback gone, an absent chain would read
// as "no search directories" and could hide a header that shadows a relocated
// include, so it is rejected instead.
//
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: grep -v '^//' %s > %t.dir/t.c
// RUN: cp -R %S/headers/refold_map_requires_include_search_chain %t.dir/headers
// RUN: cd %t.dir && clang -E -P -nostdinc -I no_such_dir -iquote also_missing \
// RUN:   --refold-map=%t.dir/map.json t.c -o %t.dir/a.i
// RUN: sed -e 's/int p = 1;/int p = 10;/' %t.dir/a.i > %t.dir/b.i
//
// Clang dropped both directories, so the producer recorded an empty chain.
// RUN: python3 -c "import json, sys; c = json.load(open(sys.argv[1]))['pp_ctx']; \
// RUN:   assert c['include_search_chain'] == [] and 'no_such_dir' in c['argv']" \
// RUN:   %t.dir/map.json
//
// The empty chain refolds.  The parent is materialized, and the clean child
// include is re-proved from the output's location with no search directory.
// RUN: cd %t.dir && clang-refold --strict --verify-output=fatal --pp a.i \
// RUN:   --pp-mod b.i --refold-map map.json --out ok.c
// RUN: FileCheck --check-prefix=OK --match-full-lines --input-file=%t.dir/ok.c \
// RUN:   %s
// OK:      int p = 10;
// OK-NEXT: #include "headers/child.h"
// OK-NEXT: int q = 2;
// OK-NEXT: int main(void) { return p + q + (v[0] != 0); }
//
// RUN: python3 -c "import json, sys; m = json.load(open(sys.argv[1])); \
// RUN:   m['pp_ctx'].pop('include_search_chain'); \
// RUN:   json.dump(m, open(sys.argv[2], 'w'))" %t.dir/map.json %t.dir/absent.json
// RUN: cd %t.dir && not clang-refold --strict --pp a.i --pp-mod b.i \
// RUN:   --refold-map absent.json --out absent.c 2>&1 \
// RUN:   | FileCheck --check-prefix=ABSENT %s
// ABSENT: failed to parse refold '{{.*}}absent.json' JSON file: Missing required field: $.pp_ctx.include_search_chain
#include "headers/parent.h"
int main(void) { return p + q + (v[0] != 0); }
