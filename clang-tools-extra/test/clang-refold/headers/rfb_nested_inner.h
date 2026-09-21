#include "rfb_nested_types.h"
#include "rfb_nested_intn.h"
#if RFB_NESTED_WIDTH == 64
typedef long rfb_nested_intptr;
#else
typedef int rfb_nested_intptr;
#endif
int rfb_nested_keep = 1;
