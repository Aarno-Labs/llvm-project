int p = 1;
// Quoted, and `outer/` is not on the include path, so this resolves only
// relative to parent.h itself.  A materialized parent therefore cannot preserve
// it, which is what sets the subtree-wide include-next force flag.
#include "mid.h"
int q = 2;
