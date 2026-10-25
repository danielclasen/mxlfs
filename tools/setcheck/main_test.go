package main

import (
	"encoding/binary"
	"strings"
	"testing"
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
	for _, in := range []string{"", "0", "50/0", "-25", "x/y"} {
		if _, err := parseRate(in); err == nil {
			t.Fatalf("%q accepted", in)
		}
	}
}
