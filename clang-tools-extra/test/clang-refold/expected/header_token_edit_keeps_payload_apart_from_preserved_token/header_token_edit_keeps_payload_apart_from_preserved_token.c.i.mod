struct s { int v; };
typedef struct s s;
typedef s *p;
int f(p t);
int f(p t) { return t->v; }
