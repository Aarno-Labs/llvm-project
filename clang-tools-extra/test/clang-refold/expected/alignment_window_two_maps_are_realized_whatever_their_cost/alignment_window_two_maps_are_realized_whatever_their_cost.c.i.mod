int run_tail(int argc, char **argv, int is_test);
                                                          
           
             
             
  
typedef struct {
  int x;
} after_t;
int use(after_t *p) { return p->x + (512 - 256); }
