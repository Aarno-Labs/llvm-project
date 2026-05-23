#define LOC 900 "logical_arg_header_ownline.c"
#define ID(x) x
#line LOC
int a = 1;
int observed = ID(
    __LINE__
);
