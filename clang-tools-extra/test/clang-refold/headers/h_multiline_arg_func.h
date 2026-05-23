#define LOC(n, f) n f
#define ID(x) x
#line LOC(1100, "logical_arg_header_ownline_func.c")
int a = 1;
int observed = ID(
    __LINE__
);
