int pre_outer_post(void) { return 5; }
int main(void) {
  return pre_outer_post() == 5 ? 0 : 1;
}
