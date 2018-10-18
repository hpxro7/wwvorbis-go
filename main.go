package main

// #cgo CFLAGS: -Wall
// #cgo LDFLAGS: -lvorbis -logg -lvorbisfile -lm
// #include <stdlib.h>
// #include "streamfile.h"
// #include "vgmstream.h"
// #include "meta.h"
import "C"

import (
	"fmt"
	"log"
	"unsafe"
)

const inputFilename = "test.wem"

func main() {
	fmt.Println("Decoding Wwise Vorbis file...")

	desc := (*C.char)(C.malloc(C.sizeof_char * 0x100))
	defer C.free(unsafe.Pointer(desc))
	f := C.open_stdio_streamfile(C.CString(inputFilename))
	if f == nil {
		log.Fatal("Could not open:", inputFilename)
	}

	s := C.init_vgmstream_wwise(f)
	if s == nil {
		log.Fatal("Could not decode as Wwise Vorbis:", inputFilename)
	}
	C.describe_vgmstream(s, (*C.char)(desc), 0x100)
	fmt.Println(C.GoString(desc))
}
