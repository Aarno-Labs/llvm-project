#if __has_embed("rmhe_data.txt")
#define RMHE_V 1
#else
#define RMHE_V 2
#endif
int rmhe_a = 1;
int rmhe_v = RMHE_V;
