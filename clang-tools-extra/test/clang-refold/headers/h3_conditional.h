#ifndef RF_H3_COND_H
#define RF_H3_COND_H

#include "common.h"

/*BOUNDARY:H3:IF-GROUP:BEGIN*/
#if defined(RF_CFG_A)
/*BOUNDARY:H3:IF-ARM:BEGIN*/
RF_MARK(h3_if_begin)
RF_PAYLOAD(h3_if)
/*BOUNDARY:H3:IF-ARM:END*/
#elif defined(RF_CFG_B)
/*BOUNDARY:H3:ELIF-ARM:BEGIN*/
RF_MARK(h3_elif_begin)
RF_PAYLOAD(h3_elif)
/*BOUNDARY:H3:ELIF-ARM:END*/
#else
/*BOUNDARY:H3:ELSE-ARM:BEGIN*/
RF_MARK(h3_else_begin)
RF_PAYLOAD(h3_else)
/*BOUNDARY:H3:ELSE-ARM:END*/
#endif
/*BOUNDARY:H3:IF-GROUP:END*/

#endif
