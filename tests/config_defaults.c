#include "../src/configfile.h"
#include <stdio.h>

int main(void) {
  g_autofree char *defaults = runtime_config_defaults();
  return fputs(defaults, stdout) == EOF ? 1 : 0;
}
