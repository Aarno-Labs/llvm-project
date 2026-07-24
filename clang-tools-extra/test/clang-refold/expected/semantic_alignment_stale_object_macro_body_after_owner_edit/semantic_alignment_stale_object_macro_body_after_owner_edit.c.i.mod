int prefix_marker = 17;
static int character_classes_refolded[4] = {0, 1, 1, 1};
static int classify_ascii_refolded(const unsigned char *buffer) { int flag = character_classes_refolded[buffer[0]]; return !(flag != 1); }
int suffix_marker = 23;
