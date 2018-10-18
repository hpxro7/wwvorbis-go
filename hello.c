#include "hello.h"
#include <stdio.h>

int say_hello(char *out) {
  return sprintf(out, "Hello!");
}
