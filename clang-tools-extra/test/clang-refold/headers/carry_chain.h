#define INNER(v) ((v) + 1)
#define OUTER(v) INNER(v)
int before = 1;
#include "carry_chain_child.h"
int after = 2;
