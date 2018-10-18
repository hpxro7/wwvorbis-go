package main

// #cgo CFLAGS: -Wall
// #include <stdlib.h>
// #include "hello.h"
import "C"
import (
	"fmt"
)

func main() {
	p := C.malloc(C.sizeof_char * 20)
	defer C.free(p)

	n := C.say_hello((*C.char)(p))
	b := C.GoBytes(p, n)
	fmt.Println(string(b))
}
