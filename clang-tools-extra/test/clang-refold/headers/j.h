#pragma once

#if defined(ADD_AND_RECORD)
#error ADD_AND_RECORD already defined
#else
extern int global_counter;

#define ADD_AND_RECORD(tag, expr)           \
  do {                                      \
    long tmp_val = (expr);                  \
    ++global_counter;                       \
  } while (0)
#endif
