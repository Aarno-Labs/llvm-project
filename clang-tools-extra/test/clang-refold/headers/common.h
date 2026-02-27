#ifndef RF_COMMON_H
#define RF_COMMON_H

// Common marker macro used to leave recognizable tokens in the PP output.
#define RF_MARK(name) int name##_marker = __LINE__;

// A macro to generate small “payload” blocks (so boundaries have code).
#define RF_PAYLOAD(tag) \
  int tag##_a = 1; \
  int tag##_b = 2;

#endif
