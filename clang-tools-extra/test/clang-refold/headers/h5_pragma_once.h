#pragma once
#include "common.h"

/*BOUNDARY:H5:BEGIN*/
RF_MARK(h5_begin)
#ifndef RF_H5_TOGGLE
#define RF_H5_TOGGLE 1
#endif

#if RF_H5_TOGGLE
  /*BOUNDARY:H5:IF-ARM:BEGIN*/
  RF_PAYLOAD(h5_if)
  /*BOUNDARY:H5:IF-ARM:END*/
#else
  /*BOUNDARY:H5:ELSE-ARM:BEGIN*/
  RF_PAYLOAD(h5_else)
  /*BOUNDARY:H5:ELSE-ARM:END*/
#endif

RF_MARK(h5_end)
/*BOUNDARY:H5:END*/
