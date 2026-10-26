// Command operate starts the procs server through Mercury, attaches the
// saturate policy with a maximum of 32 cores, and raises the number of
// callers. Start, the cpuset, and stop are Mercury requests. The server
// sets GOMAXPROCS to 32 itself.
//
// Mercury is started separately and pinned to Linux CPU 0:
//
//	mercury 8080 1 2 3 ...
//
//	go run ./operate -mercury 127.0.0.1:8080 -cpu 2
package main

import (
	"bytes"
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"
)

const (
	listen = "127.0.0.1:19091"
	daemon = "gomaxprocs32"
)

func serverBinary() string {
	if exe, err := os.Executable(); err == nil {
		if exe, err = filepath.EvalSymlinks(exe); err == nil {
			dir := filepath.Dir(exe)
			if filepath.Base(dir) == "bin" {
				return filepath.Join(filepath.Dir(dir), "bin", "server")
			}
		}
	}
	if _, file, _, ok := runtime.Caller(0); ok {
		// operate/main.go lives one directory under gomaxprocs.
		return filepath.Join(filepath.Dir(filepath.Dir(file)), "bin", "server")
	}
	fmt.Fprintln(os.Stderr, "cannot find gomaxprocs/bin/server")
	os.Exit(1)
	return ""
}

func main() {
	mercury := flag.String("mercury", "127.0.0.1:8080", "Mercury host:port")
	cpu := flag.Int("cpu", 0, "daemon CPU number Mercury was given (not the CPU Mercury pinned itself to)")
	flag.Parse()
	if flag.NArg() != 0 || *cpu < 1 {
		fmt.Fprintf(os.Stderr, "usage: operate -mercury 127.0.0.1:8080 -cpu <daemon-cpu>\n")
		os.Exit(2)
	}
	if err := run(*mercury, *cpu); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run(mercury string, cpu int) error {
	base := mercuryURL(mercury)
	if err := ping(base); err != nil {
		return err
	}
	if err := portFree(listen); err != nil {
		return err
	}

	stop := func() {
		if err := kill(base); err != nil {
			fmt.Fprintf(os.Stderr, "kill %s: %v\n", daemon, err)
		}
	}
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGINT, syscall.SIGTERM)
	go func() {
		<-sig
		stop()
		os.Exit(1)
	}()

	pid, err := start(base, cpu)
	if err != nil {
		return err
	}
	fmt.Printf("started %s pid %d\n", daemon, pid)
	defer stop()
	if err := waitPort(listen, 30*time.Second); err != nil {
		return err
	}

	if err := setCpus(base, "1"); err != nil {
		return err
	}
	attached, err := enableSaturate(base)
	if err != nil {
		return err
	}
	fmt.Printf("policy saturate min %d max %d\n", attached.Min, attached.Max)
	if err := pinTasks([]int{60, 61, 62, 63}); err != nil {
		return err
	}
	runtime.GOMAXPROCS(4)
	return ramp(listen)
}

type scaleAttach struct {
	OK  bool `json:"ok"`
	Min int  `json:"min"`
	Max int  `json:"max"`
}

func enableSaturate(base string) (scaleAttach, error) {
	status, body, err := post(base, "/scale", map[string]any{
		"daemon_id": daemon,
		"policy":    "saturate",
		"min":       1,
		"max":       32,
	})
	if err != nil {
		return scaleAttach{}, err
	}
	var attached scaleAttach
	if status != http.StatusOK || json.Unmarshal(body, &attached) != nil || !attached.OK {
		return scaleAttach{}, fmt.Errorf("POST /scale: %d %s", status, strings.TrimSpace(string(body)))
	}
	return attached, nil
}

func countCpus(spec string) (int, error) {
	spec = strings.TrimSpace(spec)
	if spec == "" {
		return 0, fmt.Errorf("empty cpuset")
	}
	total := 0
	for _, part := range strings.Split(spec, ",") {
		part = strings.TrimSpace(part)
		bounds := strings.Split(part, "-")
		switch len(bounds) {
		case 1:
			if _, err := strconv.Atoi(bounds[0]); err != nil {
				return 0, fmt.Errorf("bad cpuset %q", spec)
			}
			total++
		case 2:
			first, err1 := strconv.Atoi(bounds[0])
			last, err2 := strconv.Atoi(bounds[1])
			if err1 != nil || err2 != nil || last < first {
				return 0, fmt.Errorf("bad cpuset %q", spec)
			}
			total += last - first + 1
		default:
			return 0, fmt.Errorf("bad cpuset %q", spec)
		}
	}
	return total, nil
}

func cpuSpec(cores int) string {
	if cores <= 1 {
		return "1"
	}
	return "1-" + strconv.Itoa(cores)
}

type report struct {
	before     int
	gomaxprocs int
	numcpu     int
	allowed    string
}

func parseReport(body string) (report, error) {
	var out report
	seen := map[string]bool{}
	for _, line := range strings.Split(body, "\n") {
		fields := strings.Fields(line)
		if len(fields) < 2 {
			continue
		}
		seen[fields[0]] = true
		switch fields[0] {
		case "gomaxprocs_before":
			n, err := strconv.Atoi(fields[1])
			if err != nil {
				return report{}, fmt.Errorf("gomaxprocs_before %q", fields[1])
			}
			out.before = n
		case "gomaxprocs":
			n, err := strconv.Atoi(fields[1])
			if err != nil {
				return report{}, fmt.Errorf("gomaxprocs %q", fields[1])
			}
			out.gomaxprocs = n
		case "numcpu":
			n, err := strconv.Atoi(fields[1])
			if err != nil {
				return report{}, fmt.Errorf("numcpu %q", fields[1])
			}
			out.numcpu = n
		case "cpus_allowed_list":
			out.allowed = fields[1]
		}
	}
	if !seen["gomaxprocs_before"] || !seen["gomaxprocs"] || !seen["numcpu"] || !seen["cpus_allowed_list"] {
		return report{}, fmt.Errorf("report %q", strings.TrimSpace(body))
	}
	return out, nil
}

func getReport() (report, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, "http://"+listen+"/", nil)
	if err != nil {
		return report{}, err
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return report{}, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<16))
	if err != nil {
		return report{}, err
	}
	if resp.StatusCode != http.StatusOK {
		return report{}, fmt.Errorf("GET /: %s %s", resp.Status, bytes.TrimSpace(body))
	}
	return parseReport(string(body))
}

func mercuryURL(addr string) string {
	if strings.Contains(addr, "://") {
		return strings.TrimRight(addr, "/")
	}
	return "http://" + addr
}

func ping(base string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, base+"/ping", nil)
	if err != nil {
		return err
	}
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return fmt.Errorf("mercury %s is not answering: %w", base, err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 64))
	if resp.StatusCode != http.StatusOK || !bytes.Contains(body, []byte("pong")) {
		return fmt.Errorf("mercury %s /ping: %s %s", base, resp.Status, bytes.TrimSpace(body))
	}
	return nil
}

func start(base string, cpu int) (int, error) {
	var out struct {
		Pid int `json:"pid"`
	}
	status, body, err := post(base, "/start", map[string]any{
		"daemon_id": daemon,
		"exec_path": serverBinary(),
		"cpu":       cpu,
	})
	if err != nil {
		return 0, err
	}
	if status != http.StatusOK || json.Unmarshal(body, &out) != nil || out.Pid <= 0 {
		return 0, fmt.Errorf("POST /start: %d %s", status, strings.TrimSpace(string(body)))
	}
	return out.Pid, nil
}

func setCpus(base, spec string) error {
	status, body, err := post(base, "/cpus", map[string]string{
		"daemon_id": daemon,
		"cpus":      spec,
	})
	if err != nil {
		return err
	}
	if status != http.StatusOK || !bytes.Contains(body, []byte(`"ok":true`)) {
		return fmt.Errorf("POST /cpus %s: %d %s", spec, status, bytes.TrimSpace(body))
	}
	return nil
}

func kill(base string) error {
	status, body, err := post(base, "/kill", map[string]string{"daemon_id": daemon})
	if err != nil {
		return err
	}
	if status != http.StatusOK {
		return fmt.Errorf("POST /kill: %d %s", status, strings.TrimSpace(string(body)))
	}
	fmt.Printf("killed %s\n", daemon)
	return nil
}

func post(base, path string, payload any) (int, []byte, error) {
	raw, err := json.Marshal(payload)
	if err != nil {
		return 0, nil, err
	}
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, base+path, bytes.NewReader(raw))
	if err != nil {
		return 0, nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return 0, nil, err
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 1<<20))
	return resp.StatusCode, body, err
}

func portFree(addr string) error {
	c, err := net.DialTimeout("tcp", addr, 200*time.Millisecond)
	if err != nil {
		return nil
	}
	c.Close()
	return fmt.Errorf("%s is already in use", addr)
}

func waitPort(addr string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	var last error
	for time.Now().Before(deadline) {
		c, err := net.DialTimeout("tcp", addr, 200*time.Millisecond)
		if err == nil {
			c.Close()
			return nil
		}
		last = err
		time.Sleep(100 * time.Millisecond)
	}
	return fmt.Errorf("timed out waiting for %s: %v", addr, last)
}
