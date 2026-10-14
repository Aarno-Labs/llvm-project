struct s { int v; };
typedef struct s s;
int f(s *t);
int f(s *t) { return t->v; }
