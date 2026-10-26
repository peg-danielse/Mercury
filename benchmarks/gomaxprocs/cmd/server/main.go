// Command server sets GOMAXPROCS to 32 at startup, then reports that value,
// the value Go had already chosen, and the CPUs the kernel currently allows.
//
// Mercury starts it. The operator asks Mercury to change the cpuset, then
// reads this report. The program does not change its own cpuset.
package main

import (
	"fmt"
	"net"
	"net/http"
	"os"
	"runtime"
	"strings"
)

const addr = "127.0.0.1:19091"

func main() {
	// Go has already set GOMAXPROCS from the affinity mask. This call replaces
	// that choice. The returned value is the one it had chosen.
	before := runtime.GOMAXPROCS(32)
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintf(w, "gomaxprocs_before %d\ngomaxprocs %d\nnumcpu %d\ncpus_allowed_list %s\n",
			before, runtime.GOMAXPROCS(0), runtime.NumCPU(), cpusAllowed())
	})
	http.HandleFunc("/work", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintf(w, "ok %x\n", burn())
	})
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	if err := http.Serve(ln, nil); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func burn() uint64 {
	x := uint64(0x9E3779B97F4A7C15)
	for i := 0; i < 5000000; i++ {
		x = x*6364136223846793005 + 1
	}
	return x
}

func cpusAllowed() string {
	data, err := os.ReadFile("/proc/self/status")
	if err != nil {
		return err.Error()
	}
	for _, line := range strings.Split(string(data), "\n") {
		const prefix = "Cpus_allowed_list:"
		if strings.HasPrefix(line, prefix) {
			return strings.TrimSpace(line[len(prefix):])
		}
	}
	return "missing"
}
