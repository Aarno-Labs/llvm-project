#ifndef RF_H7_EMPTY_ARM_H
#define RF_H7_EMPTY_ARM_H

#include "common.h"

/*BOUNDARY:H7:IF-GROUP:BEGIN*/
#if defined(RF_EMPTY_ARM)
/*BOUNDARY:H7:IF-ARM:BEGIN*/
/* intentionally empty */
/*BOUNDARY:H7:IF-ARM:END*/
#else
/*BOUNDARY:H7:ELSE-ARM:BEGIN*/
RF_PAYLOAD(h7_else)
/*BOUNDARY:H7:ELSE-ARM:END*/
#endif
/*BOUNDARY:H7:IF-GROUP:END*/

#endif
