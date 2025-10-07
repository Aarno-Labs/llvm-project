void hello(const char *str);
void world(int x, float y);
int first(int x);
int last(int x);
void hello(const char *str);
#define BAR(X) X
void world(int x, float y);

int main() {
  return BAR(2);
}
