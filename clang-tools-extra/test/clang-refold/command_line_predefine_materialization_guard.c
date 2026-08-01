// RUN: %clang-refold-tester-clang-flags command_line_predefine_materialization_guard -- -I headers/command_line_predefine -DAA=1 -DBB=2
// Forward-guard: -D command-line predefines (AA/BB) coexist with an edit inside
// a materialized include. The command-line #define directives must stay
// excluded from the owner-state source-order graph (like <built-in>), so the
// materialization refold is unaffected. See RefoldOwnerStateProof
// IsVirtualInitialMacroDirectiveSource. This locks the interaction; it does not
// discriminate that fix, which is output-invariant (the false nodes are masked
// downstream by path/token comparability).
int a = AA;
#include "vals.h"
int b = BB;
