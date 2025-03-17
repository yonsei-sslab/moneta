package moneta

import "os"

// import "fmt"

// GOVMARCH holds the custom architecture variable
var MonetaVMArch string

func or(s1, s2 string) string {
	if s1 != "" {
		return s1
	}
	return s2
}

func init() {
	// Initialize GOVMARCH from an environment variable or set a default value
	MonetaVMArch = or(os.Getenv("MONETAVMARCH"), "default")

	// fmt.Printf("The custom virtual machine architecture is: %s\n", customruntime.GOVMARCH)
	// fmt.Printf("pkg moneta: MonetaVMArch: %s, addr: %p\n", MonetaVMArch, &MonetaVMArch)
}
