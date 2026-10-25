package main

import (
	"encoding/binary"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/qvest-digital/go-mxl/mxl"
)

// A view switching a grain between members changes whole pages, so what a
// reader can be served is a grain whose pages come from different indices.
// Only the full-content check tells that from a good grain: the stamp in
// the first eight bytes is the same either way.
func TestMismatchFindsAPageFromAnotherIndex(t *testing.T) {
	const size = 5 << 20
	good := make([]byte, size)
	fill(good, 1000)
	binary.LittleEndian.PutUint64(good, 1<<56|1000)
	if o := mismatch(good, 1000); o != -1 {
		t.Fatalf("a whole grain differs at %d", o)
	}

	stale := make([]byte, size)
	fill(stale, 1000-300) // what the ring held one period earlier
	torn := append([]byte(nil), good...)
	copy(torn[3*4096:4*4096], stale[3*4096:4*4096])
	if o := mismatch(torn, 1000); o != 3*4096 {
		t.Fatalf("a stale fourth page reported at %d", o)
	}
}

// A reader handed one channel's samples in place of another's, as an
// audio view selecting the wrong page range would do, must count them as
// wrong. With the index alone in a sample every channel reads the same.
func TestSampleTellsChannelsApart(t *testing.T) {
	const idx = 86058450279360
	for ch := uint64(1); ch < 16; ch++ {
		if sample(1, 0, idx)&indexMask == sample(1, ch, idx)&indexMask {
			t.Fatalf("channel %d carries the same sample as channel 0", ch)
		}
	}
	if sample(1, 3, idx)&indexMask != sample(2, 3, idx)&indexMask {
		t.Fatal("the member changes the index bits")
	}
}

// NTSC-family rates are the ones whose grain index does not land on a
// whole number of nanoseconds, so they have to reach the flow as a ratio.
func TestParseRate(t *testing.T) {
	for in, want := range map[string]string{
		"50":         `"grain_rate":{"numerator":50,"denominator":1}`,
		"60000/1001": `"grain_rate":{"numerator":60000,"denominator":1001}`,
	} {
		r, err := parseRate(in)
		if err != nil {
			t.Fatalf("%s: %v", in, err)
		}
		if def := flowDef("x", "video", geometry{width: 1280, height: 720, rate: r}); !strings.Contains(def, want) {
			t.Fatalf("%s: %s lacks %s", in, def, want)
		}
	}
	for _, in := range []string{"", "0", "50/0", "-25", "x/y", "50abc", "60000/1001x"} {
		if _, err := parseRate(in); err == nil {
			t.Fatalf("%q accepted", in)
		}
	}
}

const testID = "44444444-4444-4444-8444-444444444444"

// newWriter creates a data flow in a fresh domain under /dev/shm.
func newWriter(t *testing.T) (*mxl.Instance, *mxl.Writer, string) {
	t.Helper()
	dir, err := os.MkdirTemp("/dev/shm", "setcheck-*")
	if err != nil {
		t.Skipf("no domain in /dev/shm: %v", err)
	}
	t.Cleanup(func() { os.RemoveAll(dir) })
	inst, err := mxl.NewInstance(dir, "")
	if err != nil {
		t.Fatalf("instance: %v", err)
	}
	t.Cleanup(func() { inst.Close() })
	w, _, err := inst.NewWriter(flowDef(testID, "data", geometry{rate: mxl.Rational{Num: 50, Den: 1}}))
	if err != nil {
		t.Fatalf("writer: %v", err)
	}
	t.Cleanup(func() { w.Close() })
	return inst, w, dir
}

// readFor runs read for duration and fails the test if it does not
// return within a few seconds of it.
func readFor(t *testing.T, inst *mxl.Instance, duration time.Duration) bool {
	t.Helper()
	type result struct {
		bad bool
		err error
	}
	done := make(chan result, 1)
	go func() {
		bad, err := read(inst, testID, duration, make(chan os.Signal))
		done <- result{bad, err}
	}()
	select {
	case r := <-done:
		if r.err != nil {
			t.Fatalf("read: %v", r.err)
		}
		return r.bad
	case <-time.After(duration + 5*time.Second):
		t.Fatalf("read ran on past its %s", duration)
		return false
	}
}

// A merged output that never publishes a head is a run that read
// nothing, and the run still has to end when its duration does.
func TestReadEndsWithoutAHead(t *testing.T) {
	inst, _, _ := newWriter(t)
	if readFor(t, inst, 300*time.Millisecond) {
		t.Fatal("a reader that saw nothing reported a fault")
	}
}

// A flow replaced underneath the reader that does not come back before
// the run ends is still a reader that had to reopen, which a hitless
// run never does.
func TestReadCountsAFlowThatDoesNotComeBack(t *testing.T) {
	inst, w, dir := newWriter(t)
	g, err := w.OpenGrain(mxl.CurrentIndex(w.Config().Common.GrainRate))
	if err != nil {
		t.Fatalf("open grain: %v", err)
	}
	if err := g.Commit(anc, 0); err != nil {
		t.Fatalf("commit: %v", err)
	}
	go func() {
		time.Sleep(300 * time.Millisecond)
		os.RemoveAll(filepath.Join(dir, testID+".mxl-flow"))
	}()
	if !readFor(t, inst, 1500*time.Millisecond) {
		t.Fatal("a flow removed underneath the reader went uncounted")
	}
}
