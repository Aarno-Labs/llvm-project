int run_tail(int argc, char **argv, int is_test);
static __attribute__((unused)) const char *tag_name[3] = {
    "free",
    "object",
    "string",
};
typedef struct {
  int x;
} after_t;
int use(after_t *p) { return p->x + (512 - 256); }
