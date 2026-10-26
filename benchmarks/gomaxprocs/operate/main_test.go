package main

import "testing"

func TestCountCpus(t *testing.T) {
	got, err := countCpus("1-4")
	if err != nil || got != 4 {
		t.Fatalf("countCpus = %d, %v", got, err)
	}
	got, err = countCpus("1")
	if err != nil || got != 1 {
		t.Fatalf("countCpus = %d, %v", got, err)
	}
}

func TestCPUSpec(t *testing.T) {
	if got := cpuSpec(1); got != "1" {
		t.Fatalf("cpuSpec(1) = %q", got)
	}
	if got := cpuSpec(32); got != "1-32" {
		t.Fatalf("cpuSpec(32) = %q", got)
	}
}

func TestParseReport(t *testing.T) {
	got, err := parseReport("gomaxprocs_before 1\ngomaxprocs 32\nnumcpu 1\ncpus_allowed_list 1-4\n")
	if err != nil {
		t.Fatal(err)
	}
	if got.before != 1 || got.gomaxprocs != 32 || got.numcpu != 1 || got.allowed != "1-4" {
		t.Fatalf("%+v", got)
	}
	if _, err := parseReport("gomaxprocs 1\n"); err == nil {
		t.Fatal("expected an error")
	}
}
