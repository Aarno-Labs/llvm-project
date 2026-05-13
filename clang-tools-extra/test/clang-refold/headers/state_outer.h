#define FOO 7
int before = 1;
#include "undef_foo.h"
int after = 2;
int use = FOO;
