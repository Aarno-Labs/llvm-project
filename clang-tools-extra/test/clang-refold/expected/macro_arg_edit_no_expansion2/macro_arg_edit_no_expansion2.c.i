
extern int global_counter;

int main() {
  int x = 5, y = 2;
  do { long tmp_val = (x + y); ++global_counter; } while (0);
  return global_counter;
}
