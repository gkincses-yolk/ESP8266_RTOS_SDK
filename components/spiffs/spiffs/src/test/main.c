#include <stdlib.h>

#ifndef NO_TEST
#include "testrunner.h"
#endif

int main(int argc, char **args)
{
    //ESP_LOGF("FUNC", "main");

#ifndef NO_TEST
  run_tests(argc, args);
#endif
  exit(EXIT_SUCCESS);
}
