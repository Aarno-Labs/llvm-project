extern int *tail_location(void);
static int alloc_mode = 0;
static int free_mode = 0;
static int escape_slashes = 1;
static char *float_format = 0;
typedef int parson_bool_t;
int use(const char *s) { while (((*(s)) == ' ')) { (s)++; }; return escape_slashes + *s; }
