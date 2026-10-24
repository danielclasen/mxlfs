package main

import (
	"encoding/binary"
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
