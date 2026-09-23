extern int *tail_location(void);
typedef _Atomic(int) atomic_i32_t;
static atomic_i32_t escape_slashes = 1;
typedef int parson_bool_t;
int use(const char *s) { while (((*(s)) == ' ')) { (s)++; }; return escape_slashes + *s; }
