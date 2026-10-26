package main

import (
	"context"
	"fmt"
	"io"
	"math"
	"net/http"
	"os"
	"sort"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

const (
	rampSteps   = 6
	rampWindows = 4
)

func ramp(addr string) error {
	client := &http.Client{Transport: &http.Transport{
		MaxIdleConns:        64,
		MaxIdleConnsPerHost: 64,
		MaxConnsPerHost:     64,
		DisableCompression:  true,
	}}
	workURL := "http://" + addr + "/work"
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	var active atomic.Int32
	var sent, done, errs atomic.Uint64
	var rec recorder
	var wg sync.WaitGroup
	for id := 0; id < 32; id++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			for ctx.Err() == nil {
				if int32(id) >= active.Load() {
					time.Sleep(5 * time.Millisecond)
					continue
				}
				start := time.Now()
				sent.Add(1)
				reqCtx, reqCancel := context.WithTimeout(ctx, 5*time.Second)
				req, err := http.NewRequestWithContext(reqCtx, http.MethodGet, workURL, nil)
				if err != nil {
					reqCancel()
					errs.Add(1)
					continue
				}
				resp, err := client.Do(req)
				if err == nil {
					io.Copy(io.Discard, io.LimitReader(resp.Body, 1<<16))
					resp.Body.Close()
					if resp.StatusCode != http.StatusOK {
						err = fmt.Errorf("status %d", resp.StatusCode)
					}
				}
				reqCancel()
				if ctx.Err() != nil {
					return
				}
				if err != nil {
					errs.Add(1)
					continue
				}
				done.Add(1)
				rec.add(uint64(time.Since(start)))
			}
		}(id)
	}
	defer func() {
		cancel()
		wg.Wait()
	}()

	fmt.Println("clients cores cpus gomaxprocs arrival_rate service_rate avg_us p95_us errors")
	clients := 1
	for step := 0; step < rampSteps; step++ {
		active.Store(int32(clients))
		for window := 0; window < rampWindows; window++ {
			beforeSent, beforeDone, beforeErr := sent.Load(), done.Load(), errs.Load()
			rec.arm()
			began := time.Now()
			time.Sleep(time.Second)
			elapsed := time.Since(began)
			samples := rec.disarm()
			report, err := getReport()
			if err != nil {
				return err
			}
			cores, err := countCpus(report.allowed)
			if err != nil {
				return err
			}
			arrival := perSecond(sent.Load()-beforeSent, elapsed)
			service := perSecond(done.Load()-beforeDone, elapsed)
			fmt.Printf("%d %d %s %d %d %d %.1f %.1f %d\n",
				clients, cores, report.allowed, report.gomaxprocs,
				arrival, service, averageUs(samples), p95Us(samples), errs.Load()-beforeErr)
		}
		clients *= 2
	}
	return nil
}

func perSecond(count uint64, elapsed time.Duration) uint64 {
	if elapsed <= 0 || count == 0 {
		return 0
	}
	return count * uint64(time.Second) / uint64(elapsed)
}

func averageUs(samples []uint64) float64 {
	if len(samples) == 0 {
		return 0
	}
	var sum uint64
	for _, sample := range samples {
		sum += sample
	}
	return float64(sum) / float64(len(samples)) / float64(time.Microsecond)
}

func p95Us(samples []uint64) float64 {
	if len(samples) == 0 {
		return 0
	}
	sorted := append([]uint64(nil), samples...)
	sort.Slice(sorted, func(i, j int) bool { return sorted[i] < sorted[j] })
	rank := int(math.Ceil(0.95*float64(len(sorted)))) - 1
	if rank < 0 {
		rank = 0
	}
	if rank >= len(sorted) {
		rank = len(sorted) - 1
	}
	return float64(sorted[rank]) / float64(time.Microsecond)
}

type recorder struct {
	mu      sync.Mutex
	on      bool
	samples []uint64
}

func (r *recorder) arm() {
	r.mu.Lock()
	r.samples = r.samples[:0]
	r.on = true
	r.mu.Unlock()
}

func (r *recorder) disarm() []uint64 {
	r.mu.Lock()
	r.on = false
	out := append([]uint64(nil), r.samples...)
	r.mu.Unlock()
	return out
}

func (r *recorder) add(ns uint64) {
	r.mu.Lock()
	if r.on {
		r.samples = append(r.samples, ns)
	}
	r.mu.Unlock()
}

func pinTasks(cpus []int) error {
	var mask [16]uint64
	for _, cpu := range cpus {
		if cpu < 0 || cpu >= len(mask)*64 {
			return fmt.Errorf("cpu %d", cpu)
		}
		mask[cpu/64] |= 1 << uint(cpu%64)
	}
	entries, err := os.ReadDir("/proc/self/task")
	if err != nil {
		return err
	}
	for _, entry := range entries {
		var tid int
		if _, err := fmt.Sscan(entry.Name(), &tid); err != nil {
			continue
		}
		_, _, errno := syscall.RawSyscall(syscall.SYS_SCHED_SETAFFINITY, uintptr(tid), uintptr(len(mask)*8), uintptr(unsafe.Pointer(&mask[0])))
		if errno != 0 {
			return fmt.Errorf("pin tid %d: %w", tid, errno)
		}
	}
	return nil
}
