#include "tui.h"

int main(void) {
  tui_init();

  TMT *tmt = tui_globalcontext.screen;

  tui_deinit();
}

/*
int main(void) {
  return example_main();
}
*/
