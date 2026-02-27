int pre_inner_post(void) { return 5; }
int main(void) {
  return pre_inner_post() == 5 ? 0 : 1;
}
