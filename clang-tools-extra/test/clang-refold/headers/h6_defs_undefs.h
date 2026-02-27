#ifndef RF_H6_DEFS_UNDEFS_H
#define RF_H6_DEFS_UNDEFS_H

#include "common.h"

/*BOUNDARY:H6:BEGIN*/
#define RF_H6_LOCAL(x) x
RF_MARK(h6_after_define)

/* boundary around undef */
#undef RF_H6_LOCAL
RF_MARK(h6_after_undef)
/*BOUNDARY:H6:END*/

#endif
