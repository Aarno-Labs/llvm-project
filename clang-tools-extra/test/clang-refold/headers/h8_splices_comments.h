#ifndef RF_H8_SPLICES_COMMENTS_H
#define RF_H8_SPLICES_COMMENTS_H

#include "common.h"

/*BOUNDARY:H8:BEGIN*/
// The following uses line splices and comments that can confuse naive scanners.
#define RF_H8_CALLER_FORMAL_0 foo\
  /*comment*/bar
#define RF_H8_CALLER_FORMAL_1 baz

RF_MARK(h8_begin)
RF_PAYLOAD(h8)
RF_MARK(h8_end)
/*BOUNDARY:H8:END*/

#endif
