package main

// #cgo CFLAGS: -Wall
// #cgo LDFLAGS: -lvorbis -logg -lvorbisfile -lm
// #include <stdlib.h>
// #include "hello.h"
// #include "streamfile.h"
// #include "vgmstream.h"
// #include "meta.h"
import "C"

import (
	"fmt"
	"unsafe"
)

func main() {
	p := C.malloc(C.sizeof_char * 20)
	defer C.free(p)

	n := C.say_hello((*C.char)(p))
	b := C.GoBytes(p, n)
	fmt.Println(string(b))

	desc := (*C.char)(C.malloc(C.sizeof_char * 0x100))
	defer C.free(unsafe.Pointer(desc))
	f := C.open_stdio_streamfile(C.CString("test.wem"))
	fmt.Printf("Streamfile: Type=%T, Value=%v\n\n", f, f)
	s := C.init_vgmstream_wwise(f)
	C.describe_vgmstream(s, (*C.char)(desc), 0x100)
	fmt.Println("Descriptor: ", C.GoString(desc))
}
