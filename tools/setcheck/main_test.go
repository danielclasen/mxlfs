package main

import (
	"encoding/binary"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
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

func TestCheckWrite(t *testing.T) {
	for _, f := range []string{"video", "data", "audio"} {
		if err := checkWrite(f, 7); err != nil {
			t.Errorf("checkWrite(%q, 7): %v", f, err)
		}
	}
	// A typo must not write a video flow, and member 8 would set the
	// audio sample mark and read back as member 0.
	for _, c := range []struct {
		format string
		member int
	}{{"vidoe", 0}, {"", 0}, {"video", 8}, {"audio", -1}} {
		if err := checkWrite(c.format, c.member); err == nil {
			t.Errorf("checkWrite(%q, %d) accepted", c.format, c.member)
		}
	}
}

// A flag of the other command is refused: a reader given -member, or a
// writer given -duration, would otherwise run as if it had not been.
func TestParseFlagsRefusesTheOtherCommandsFlags(t *testing.T) {
	for _, c := range []struct {
		cmd  string
		args []string
		ok   bool
	}{
		{"write", []string{"-id", "x", "-format", "audio", "-member", "3", "-rate", "60000/1001"}, true},
		{"read", []string{"-id", "x", "-duration", "1s"}, true},
		{"read", []string{"-id", "x", "-member", "1"}, false},
		{"read", []string{"-id", "x", "-format", "audio"}, false},
		{"write", []string{"-id", "x", "-duration", "1s"}, false},
		{"write", []string{"-id", "x", "extra"}, false},
		{"read", []string{"-id", "x", "extra"}, false},
		{"write", []string{"-format", "video"}, false},
		{"write", []string{"-id", "x", "-rate", "0"}, false},
		{"write", []string{"-id", "x", "-member", "8"}, false},
		{"list", []string{"-id", "x"}, false},
	} {
		if _, err := parseFlags(c.cmd, c.args); (err == nil) != c.ok {
			t.Errorf("parseFlags(%q, %q): err=%v, want ok=%v", c.cmd, c.args, err, c.ok)
		}
	}
	o, err := parseFlags("write", []string{"-id", "x", "-width", "1280", "-rate", "60000/1001"})
	if err != nil || o.width != 1280 || o.rate != (mxl.Rational{Num: 60000, Den: 1001}) || o.domain != "/run/mxl/domain" {
		t.Errorf("parseFlags write: %+v, %v", o, err)
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
		t.Fatalf("no domain in /dev/shm: %v", err)
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

// A view serving stale grains does so for every grain until it recovers,
// at 50 or 60 a second, so a reader that logged each one would bury the
// summaries failover.sh reads. The counters still count them all.
func TestReadGrainLogsTheFirstFaultsOfAKind(t *testing.T) {
	inst, w, _ := newWriter(t)
	r, err := inst.NewReader(testID)
	if err != nil {
		t.Fatalf("reader: %v", err)
	}
	defer r.Close()
	logs := &syncBuffer{}
	log.SetOutput(logs)
	t.Cleanup(func() { log.SetOutput(os.Stderr) })
	start := mxl.CurrentIndex(w.Config().Common.GrainRate)
	const faults = 3 * logFaults
	var s stats
	for i := start; i < start+faults; i++ {
		g, err := w.OpenGrain(i)
		if err != nil {
			t.Fatalf("open grain %d: %v", i, err)
		}
		fill(g.Payload[:anc], i)
		binary.LittleEndian.PutUint64(g.Payload, i+1000) // another grain's stamp
		if err := g.Commit(anc, 0); err != nil {
			t.Fatalf("commit grain %d: %v", i, err)
		}
		if err := readGrain(r, i, true, &s); err != nil {
			t.Fatalf("read grain %d: %v", i, err)
		}
	}
	if s.wrong != faults || !s.bad() {
		t.Errorf("wrong = %d, bad = %v; want %d, true", s.wrong, s.bad(), faults)
	}
	if n := strings.Count(logs.String(), " carries grain "); n != logFaults {
		t.Errorf("logged %d of %d wrong grains, want %d:\n%s", n, faults, logFaults, logs.String())
	}
}

// A reader whose clock is behind the writer's sees a grain before its
// due time, which reads as a negative latency, not a huge one. A long run
// keeps a sample of bounded size, and still the exact maximum.
func TestLatencySample(t *testing.T) {
	var s stats
	s.addLatency(-3000)
	if !strings.Contains(s.String(), " latency_us_p50=-3 p99=-3 max=-3 ") {
		t.Fatalf("one latency of -3 us reads %s", s.String())
	}
	s = stats{}
	const n = 1 << 20
	for i := int64(0); i < n; i++ {
		s.addLatency(i * 1000)
	}
	if len(s.lat) != latSample || s.latN != n || s.latMax != (n-1)*1000 {
		t.Fatalf("kept %d of %d, max %d", len(s.lat), s.latN, s.latMax)
	}
	l := slices.Clone(s.lat)
	slices.Sort(l)
	// The sample's median of 0..n-1 us lies within 2% of n/2 for any
	// realistic draw; one that kept only the first latSample stays near
	// latSample/2.
	if p50 := l[len(l)/2] / 1000; p50 < n/2-n/50 || p50 > n/2+n/50 {
		t.Fatalf("p50 %d us of 0..%d us", p50, n-1)
	}
}

// A signal ends a run without a duration, and is not an error, both
// while the reader waits in join for a flow with no head and while it
// reads in follow.
func TestReadEndsOnSignal(t *testing.T) {
	for _, grains := range []bool{false, true} {
		t.Run(fmt.Sprintf("grains=%v", grains), func(t *testing.T) {
			inst, w, _ := newWriter(t)
			logs := &syncBuffer{}
			log.SetOutput(logs)
			t.Cleanup(func() { log.SetOutput(os.Stderr) })
			quit, written := make(chan struct{}), make(chan struct{})
			go func() {
				defer close(written)
				rate := w.Config().Common.GrainRate
				for i := mxl.CurrentIndex(rate); grains; i++ {
					select {
					case <-quit:
						return
					case <-time.After(time.Duration(mxl.NsUntilIndex(i, rate))):
					}
					g, err := w.OpenGrain(i)
					if err != nil {
						return
					}
					fill(g.Payload[:anc], i)
					binary.LittleEndian.PutUint64(g.Payload, i)
					if g.Commit(anc, 0) != nil {
						return
					}
				}
			}()
			t.Cleanup(func() { close(quit); <-written })
			stop := make(chan os.Signal, 1)
			done := make(chan error, 1)
			go func() {
				_, err := read(inst, testID, 0, stop)
				done <- err
			}()
			if grains {
				// read logs this right before it calls follow.
				for deadline := time.Now().Add(5 * time.Second); !strings.Contains(logs.String(), "reading "); {
					if time.Now().After(deadline) {
						t.Fatalf("the reader never started reading: %s", logs.String())
					}
					time.Sleep(10 * time.Millisecond)
				}
			} else {
				time.Sleep(200 * time.Millisecond)
				if strings.Contains(logs.String(), "reading ") {
					t.Fatal("the reader found a head where none was written")
				}
			}
			stop <- os.Interrupt
			select {
			case err := <-done:
				if err != nil {
					t.Fatalf("read: %v", err)
				}
			case <-time.After(5 * time.Second):
				t.Fatal("read ran on after the signal")
			}
		})
	}
}

// syncBuffer collects log output written from read's goroutine.
type syncBuffer struct {
	mu sync.Mutex
	b  strings.Builder
}

func (b *syncBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.b.Write(p)
}

func (b *syncBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.b.String()
}
