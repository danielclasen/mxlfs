// Command setcheck writes and verifies flows for redundancy tests on a
// cluster.
//
//	setcheck write -domain D -id ID -format video|data|audio -member N
//	setcheck read  -domain D -id ID [-duration 0]
//
// A writer stamps every grain with its member number and index in the
// first eight payload bytes, and every audio sample with a mark, its
// member number and the low 28 bits of its index. A reader checks each
// grain or sample against the index it asked for, so it tells a stale
// picture or a gap from a good one, and prints a summary every five
// seconds and at the end: counts per outcome and per member, latency
// from the index's due time, and its own CPU time. It exits 3 when
// anything was wrong.
package main

import (
	"encoding/binary"
	"errors"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"slices"
	"syscall"
	"time"

	"github.com/qvest-digital/go-mxl/mxl"
)

const (
	batch      = 480 // samples per audio commit, 10 ms at 48 kHz
	sampleMark = 0x80000000
	indexMask  = 0x0fffffff
)

func main() {
	log.SetFlags(log.Ltime | log.Lmicroseconds)
	if len(os.Args) < 2 {
		log.Fatal("usage: setcheck write|read [flags]")
	}
	fs := flag.NewFlagSet(os.Args[1], flag.ExitOnError)
	domain := fs.String("domain", "/run/mxl/domain", "MXL domain directory")
	id := fs.String("id", "", "flow id")
	format := fs.String("format", "video", "video, data or audio (write)")
	member := fs.Int("member", 0, "member number stamped into the payload, 0-7 (write)")
	width := fs.Int("width", 1920, "video width (write)")
	height := fs.Int("height", 1080, "video height (write)")
	rate := fs.Int("rate", 50, "grain rate (write)")
	duration := fs.Duration("duration", 0, "stop after this long; 0 runs until signalled (read)")
	_ = fs.Parse(os.Args[2:])
	if *id == "" {
		log.Fatal("-id is required")
	}

	inst, err := mxl.NewInstance(*domain, "")
	if err != nil {
		log.Fatalf("instance: %v", err)
	}
	defer inst.Close()
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, syscall.SIGINT, syscall.SIGTERM)

	switch os.Args[1] {
	case "write":
		err = write(inst, flowDef(*id, *format, *width, *height, *rate), *format, uint64(*member), stop)
	case "read":
		var bad bool
		bad, err = read(inst, *id, *duration, stop)
		if err == nil && bad {
			os.Exit(3)
		}
	default:
		err = fmt.Errorf("unknown command %q", os.Args[1])
	}
	if err != nil {
		log.Fatal(err)
	}
}

func flowDef(id, format string, w, h, rate int) string {
	head := `{"description":"setcheck","id":"` + id + `","label":"setcheck","parents":[],` +
		`"tags":{"urn:x-nmos:tag:grouphint/v1.0":["setcheck:` + format + `"]},`
	switch format {
	case "data":
		return head + fmt.Sprintf(`"format":"urn:x-nmos:format:data","media_type":"video/smpte291",`+
			`"grain_rate":{"numerator":%d,"denominator":1}}`, rate)
	case "audio":
		return head + `"format":"urn:x-nmos:format:audio","media_type":"audio/float32",` +
			`"sample_rate":{"numerator":48000},"channel_count":2,"bit_depth":32}`
	}
	return head + fmt.Sprintf(`"format":"urn:x-nmos:format:video","media_type":"video/v210",`+
		`"grain_rate":{"numerator":%d,"denominator":1},"frame_width":%d,"frame_height":%d,`+
		`"interlace_mode":"progressive","colorspace":"BT709","components":[`+
		`{"name":"Y","width":%d,"height":%d,"bit_depth":10},`+
		`{"name":"Cb","width":%d,"height":%d,"bit_depth":10},`+
		`{"name":"Cr","width":%d,"height":%d,"bit_depth":10}]}`, rate, w, h, w, h, w/2, h, w/2, h)
}

func write(inst *mxl.Instance, def, format string, member uint64, stop <-chan os.Signal) error {
	w, _, err := inst.NewWriter(def)
	if err != nil {
		return fmt.Errorf("writer: %w", err)
	}
	defer w.Close()
	cfg := w.Config()
	log.Printf("writing %s as member %d", format, member)

	if cfg.Common.Format == mxl.FormatAudio {
		rate := mxl.Rational{Num: 48000, Den: 1}
		for end := (mxl.CurrentIndex(rate)/batch + 1) * batch; ; end += batch {
			select {
			case <-stop:
				return nil
			case <-time.After(time.Duration(mxl.NsUntilIndex(end, rate))):
			}
			s, err := w.OpenSamples(end, batch)
			if err != nil {
				return fmt.Errorf("open samples %d: %w", end, err)
			}
			for ch := uint64(0); ch < 2; ch++ {
				f1, f2, err := s.ChannelFragments(ch)
				if err != nil {
					return err
				}
				idx := end - batch
				for _, f := range [][]byte{f1, f2} {
					for o := 0; o+4 <= len(f); o += 4 {
						binary.LittleEndian.PutUint32(f[o:], sampleMark|uint32(member)<<28|uint32(idx&indexMask))
						idx++
					}
				}
			}
			if err := s.Commit(); err != nil {
				return err
			}
		}
	}

	rate := cfg.Common.GrainRate
	data := cfg.Common.Format == mxl.FormatData
	for i := mxl.CurrentIndex(rate); ; i++ {
		select {
		case <-stop:
			return nil
		case <-time.After(time.Duration(mxl.NsUntilIndex(i, rate))):
		}
		g, err := w.OpenGrain(i)
		if err != nil {
			return fmt.Errorf("open grain %d: %w", i, err)
		}
		binary.LittleEndian.PutUint64(g.Payload, member<<56|i)
		valid := g.TotalSlices
		if data {
			valid = 64 // a data grain's valid slices are its length in bytes
		}
		if err := g.Commit(valid, 0); err != nil {
			return err
		}
	}
}

type stats struct {
	ok, invalid, wrong, late, timeouts uint64
	silent                             uint64 // audio samples of zero
	members                            [8]uint64
	lat                                []uint64 // ns after the index's due time
}

func (s *stats) String() string {
	out := fmt.Sprintf("ok=%d invalid=%d wrong_index=%d late=%d timeouts=%d silent=%d",
		s.ok, s.invalid, s.wrong, s.late, s.timeouts, s.silent)
	for m, n := range s.members {
		if n > 0 {
			out += fmt.Sprintf(" member%d=%d", m, n)
		}
	}
	if len(s.lat) > 0 {
		l := slices.Clone(s.lat)
		slices.Sort(l)
		out += fmt.Sprintf(" latency_us_p50=%d p99=%d max=%d",
			l[len(l)/2]/1000, l[len(l)*99/100]/1000, l[len(l)-1]/1000)
	}
	var ru syscall.Rusage
	_ = syscall.Getrusage(syscall.RUSAGE_SELF, &ru)
	cpu := time.Duration(ru.Utime.Nano() + ru.Stime.Nano())
	return out + fmt.Sprintf(" cpu=%s", cpu.Round(time.Millisecond))
}

func (s *stats) bad() bool {
	return s.invalid+s.wrong+s.late+s.silent > 0
}

func read(inst *mxl.Instance, id string, duration time.Duration, stop <-chan os.Signal) (bool, error) {
	var r *mxl.Reader
	for {
		var err error
		if r, err = inst.NewReader(id); err == nil {
			break
		}
		log.Printf("reader: %v; retrying", err)
		select {
		case <-stop:
			return false, nil
		case <-time.After(time.Second):
		}
	}
	defer r.Close()
	cfg, err := r.Config()
	if err != nil {
		return false, err
	}
	var end <-chan time.Time
	if duration > 0 {
		end = time.After(duration)
	}
	tick := time.NewTicker(5 * time.Second)
	defer tick.Stop()
	s := &stats{}
	audio := cfg.Common.Format == mxl.FormatAudio
	data := cfg.Common.Format == mxl.FormatData
	rate := cfg.Common.GrainRate
	step := uint64(1)
	if audio {
		rate, step = mxl.Rational{Num: 48000, Den: 1}, batch
	}
	// Start where a reader joining the flow would: past its head, which
	// a merged output only advances over what a member delivered.
	rt, err := r.Runtime()
	if err != nil {
		return false, err
	}
	idx := (rt.HeadIndex/step + 1) * step
	log.Printf("reading %s from index %d", id, idx)
	for {
		select {
		case <-stop:
			log.Printf("final %s", s)
			return s.bad(), nil
		case <-end:
			log.Printf("final %s", s)
			return s.bad(), nil
		case <-tick.C:
			log.Printf("%s", s)
		default:
		}
		var err error
		if audio {
			err = readSamples(r, idx, s)
		} else {
			err = readGrain(r, idx, data, s)
		}
		switch {
		case err == nil:
			s.lat = append(s.lat, mxl.Now()-mxl.IndexToTimestamp(rate, idx))
			idx += step
		case errors.Is(err, mxl.ErrOutOfRangeLate):
			s.late++
			rt, _ := r.Runtime()
			log.Printf("index %d is gone, head %d", idx, rt.HeadIndex)
			idx = (rt.HeadIndex / step) * step
		case errors.Is(err, mxl.ErrTimeout), errors.Is(err, mxl.ErrOutOfRangeEarly):
			s.timeouts++
		default:
			return false, fmt.Errorf("index %d: %w", idx, err)
		}
	}
}

func readGrain(r *mxl.Reader, idx uint64, data bool, s *stats) error {
	var g mxl.Grain
	var err error
	if data {
		g, err = r.GetGrainSlice(idx, 0, 200*time.Millisecond)
	} else {
		g, err = r.GetGrain(idx, 200*time.Millisecond)
	}
	if err != nil {
		return err
	}
	if g.Invalid() {
		s.invalid++
		log.Printf("grain %d is INVALID", idx)
		return nil
	}
	v := binary.LittleEndian.Uint64(g.Payload)
	if v&(1<<56-1) != idx {
		s.wrong++
		log.Printf("grain %d carries grain %d of member %d", idx, v&(1<<56-1), v>>56)
		return nil
	}
	s.ok++
	s.members[v>>56&7]++
	return nil
}

func readSamples(r *mxl.Reader, end uint64, s *stats) error {
	v, err := r.GetSamples(end, batch, 200*time.Millisecond)
	if err != nil {
		return err
	}
	for ch := uint64(0); ch < 2; ch++ {
		buf, err := v.CopyChannel(ch)
		if err != nil {
			return err
		}
		for o := 0; o+4 <= len(buf); o += 4 {
			x := binary.LittleEndian.Uint32(buf[o:])
			idx := end - batch + uint64(o/4)
			switch {
			case x == 0:
				if s.silent == 0 {
					log.Printf("sample %d of channel %d is silence", idx, ch)
				}
				s.silent++
			case x&sampleMark == 0 || x&indexMask != uint32(idx&indexMask):
				if s.wrong == 0 {
					log.Printf("sample %d of channel %d carries %#x", idx, ch, x)
				}
				s.wrong++
			default:
				s.ok++
				s.members[x>>28&7]++
			}
		}
	}
	return nil
}
