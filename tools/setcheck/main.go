// Command setcheck writes and verifies flows for redundancy tests on a
// cluster.
//
//	setcheck write -domain D -id ID -format video|data|audio -member N
//	         [-width 1920 -height 1080 -rate 50|60000/1001]
//	         [-channels 2 -sample-rate 48000]
//	setcheck read  -domain D -id ID [-duration 0]
//
// A writer fills every byte of a grain's payload from its index alone,
// so all members write the same picture, as a redundant source does, and
// stamps its member number and the index into the first eight bytes. A
// data grain is anc bytes long. Every audio sample carries a mark, the
// member number and the low 28 bits of its index, offset per channel. A
// reader takes the geometry from the flow and checks every byte of every
// grain and every sample against the index it asked for, so a stale grain, a grain assembled from two indices or members, a
// short data grain or a gap all count against it. It prints a summary
// every five seconds and at the end: counts per outcome and per member,
// latency from the index's due time, and its own CPU time. It exits 3
// when anything was wrong.
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
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/qvest-digital/go-mxl/mxl"
)

const (
	channelStride = 1 << 20 // index offset between channels in a sample
	anc           = 64      // bytes in a data grain
	batch         = 480     // samples per audio commit, 10 ms at 48 kHz
	sampleMark    = 0x80000000
	indexMask     = 0x0fffffff
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
	rateFlag := fs.String("rate", "50", "grain rate, N or N/D such as 60000/1001 (write)")
	channels := fs.Int("channels", 2, "audio channels (write)")
	sampleRate := fs.Int("sample-rate", 48000, "audio sample rate (write)")
	duration := fs.Duration("duration", 0, "stop after this long; 0 runs until signalled (read)")
	_ = fs.Parse(os.Args[2:])
	if *id == "" {
		log.Fatal("-id is required")
	}
	rate, err := parseRate(*rateFlag)
	if err != nil {
		log.Fatal(err)
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
		g := geometry{width: *width, height: *height, rate: rate, channels: *channels, sampleRate: *sampleRate}
		err = write(inst, flowDef(*id, *format, g), *format, uint64(*member), stop)
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

// geometry is what a writer's flow looks like.
type geometry struct {
	width, height        int
	rate                 mxl.Rational
	channels, sampleRate int
}

// parseRate reads "50" or "60000/1001".
func parseRate(s string) (mxl.Rational, error) {
	num, den, ratio := strings.Cut(s, "/")
	n, err := strconv.ParseInt(num, 10, 64)
	d := int64(1)
	if err == nil && ratio {
		d, err = strconv.ParseInt(den, 10, 64)
	}
	if err != nil {
		return mxl.Rational{}, fmt.Errorf("rate %q: want N or N/D", s)
	}
	if n <= 0 || d <= 0 {
		return mxl.Rational{}, fmt.Errorf("rate %q: must be positive", s)
	}
	return mxl.Rational{Num: n, Den: d}, nil
}

func flowDef(id, format string, g geometry) string {
	head := `{"description":"setcheck","id":"` + id + `","label":"setcheck","parents":[],` +
		`"tags":{"urn:x-nmos:tag:grouphint/v1.0":["setcheck:` + format + `"]},`
	switch format {
	case "data":
		return head + fmt.Sprintf(`"format":"urn:x-nmos:format:data","media_type":"video/smpte291",`+
			`"grain_rate":{"numerator":%d,"denominator":%d}}`, g.rate.Num, g.rate.Den)
	case "audio":
		return head + fmt.Sprintf(`"format":"urn:x-nmos:format:audio","media_type":"audio/float32",`+
			`"sample_rate":{"numerator":%d},"channel_count":%d,"bit_depth":32}`, g.sampleRate, g.channels)
	}
	w, h := g.width, g.height
	return head + fmt.Sprintf(`"format":"urn:x-nmos:format:video","media_type":"video/v210",`+
		`"grain_rate":{"numerator":%d,"denominator":%d},"frame_width":%d,"frame_height":%d,`+
		`"interlace_mode":"progressive","colorspace":"BT709","components":[`+
		`{"name":"Y","width":%d,"height":%d,"bit_depth":10},`+
		`{"name":"Cb","width":%d,"height":%d,"bit_depth":10},`+
		`{"name":"Cr","width":%d,"height":%d,"bit_depth":10}]}`, g.rate.Num, g.rate.Den, w, h, w, h, w/2, h, w/2, h)
}

// sample is the audio sample at index idx of channel ch written by member:
// a mark, the member, and an index offset per channel, so a sample from
// another channel, member or position reads as wrong.
func sample(member, ch, idx uint64) uint32 {
	return sampleMark | uint32(member)<<28 | uint32((idx+ch*channelStride)&indexMask)
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
		rate := cfg.Common.GrainRate // the sample rate
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
			for ch := uint64(0); ch < uint64(cfg.Continuous.ChannelCount); ch++ {
				f1, f2, err := s.ChannelFragments(ch)
				if err != nil {
					return err
				}
				idx := end - batch
				for _, f := range [][]byte{f1, f2} {
					for o := 0; o+4 <= len(f); o += 4 {
						binary.LittleEndian.PutUint32(f[o:], sample(member, ch, idx))
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
		n := len(g.Payload)
		valid := g.TotalSlices
		if data {
			n, valid = anc, anc // a data grain's valid slices are its length in bytes
		}
		fill(g.Payload[:n], i)
		binary.LittleEndian.PutUint64(g.Payload, member<<56|i)
		if err := g.Commit(valid, 0); err != nil {
			return err
		}
	}
}

// word is the content of the payload word at offset k of grain index;
// the first word carries the member and the index instead.
func word(index, k uint64) uint64 { return index ^ (k+1)*0x9e3779b97f4a7c15 }

func fill(p []byte, index uint64) {
	for o := 0; o+8 <= len(p); o += 8 {
		binary.LittleEndian.PutUint64(p[o:], word(index, uint64(o/8)))
	}
}

// mismatch is the offset of the first payload byte after the stamp that
// is not what grain index carries, or -1.
func mismatch(p []byte, index uint64) int {
	for o := 8; o+8 <= len(p); o += 8 {
		if binary.LittleEndian.Uint64(p[o:]) != word(index, uint64(o/8)) {
			return o
		}
	}
	return -1
}

type stats struct {
	ok, invalid, wrong, late, timeouts uint64
	reopened                           uint64 // times the flow was replaced underneath the reader
	corrupt                            uint64 // grains with a byte past the stamp not their own
	silent                             uint64 // audio samples of zero
	members                            [8]uint64
	lat                                []uint64 // ns after the index's due time
}

func (s *stats) String() string {
	out := fmt.Sprintf("ok=%d invalid=%d wrong_index=%d corrupt=%d late=%d timeouts=%d silent=%d reopened=%d",
		s.ok, s.invalid, s.wrong, s.corrupt, s.late, s.timeouts, s.silent, s.reopened)
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
	return s.invalid+s.wrong+s.corrupt+s.late+s.silent+s.reopened > 0
}

func read(inst *mxl.Instance, id string, duration time.Duration, stop <-chan os.Signal) (bool, error) {
	var end <-chan time.Time
	if duration > 0 {
		end = time.After(duration)
	}
	tick := time.NewTicker(5 * time.Second)
	defer tick.Stop()
	s := &stats{}
	for opened := false; ; opened = true {
		r, cfg, idx, err := join(inst, id, end, stop)
		if err != nil {
			return false, err
		}
		if r == nil {
			log.Printf("final %s", s)
			return s.bad(), nil
		}
		if opened {
			log.Printf("reopened %s at index %d", id, idx)
		} else {
			log.Printf("reading %s from index %d", id, idx)
		}
		done, err := follow(r, cfg, idx, s, end, tick.C, stop)
		r.Close()
		if done || err != nil {
			log.Printf("final %s", s)
			return s.bad(), err
		}
		// A merged output is built again when a member changes
		// geometry or its set is recreated; every reader of it has
		// to reopen, which a hitless run never needs. It counts
		// whether or not the flow comes back before the run ends.
		s.reopened++
	}
}

// join opens the flow and returns the index after its head, where a
// reader joining the flow starts. A merged output exists before its
// first merge publishes a head, and one that is built again leaves the
// old one's head where it stopped, so a reader that sees no head for a
// second, or finds the flow replaced, opens the flow again. It returns
// no reader when the run ends first.
func join(inst *mxl.Instance, id string, end <-chan time.Time,
	stop <-chan os.Signal) (*mxl.Reader, mxl.FlowConfig, uint64, error) {
	for {
		r, err := inst.NewReader(id)
		if err != nil {
			log.Printf("reader: %v; retrying", err)
			select {
			case <-stop:
				return nil, mxl.FlowConfig{}, 0, nil
			case <-end:
				return nil, mxl.FlowConfig{}, 0, nil
			case <-time.After(time.Second):
			}
			continue
		}
		cfg, err := r.Config()
		if err != nil {
			r.Close()
			return nil, mxl.FlowConfig{}, 0, err
		}
		step := uint64(1)
		if cfg.Common.Format == mxl.FormatAudio {
			step = batch
		}
		for deadline := time.Now().Add(time.Second); time.Now().Before(deadline); {
			rt, err := r.Runtime()
			if errors.Is(err, mxl.ErrFlowInvalid) {
				break // replaced while joining: open it again
			}
			if err != nil {
				r.Close()
				return nil, mxl.FlowConfig{}, 0, err
			}
			if rt.HeadIndex != 0 {
				return r, cfg, (rt.HeadIndex/step + 1) * step, nil
			}
			select {
			case <-stop:
				r.Close()
				return nil, mxl.FlowConfig{}, 0, nil
			case <-end:
				r.Close()
				return nil, mxl.FlowConfig{}, 0, nil
			case <-time.After(10 * time.Millisecond):
			}
		}
		r.Close()
	}
}

// follow reads from idx on until the run ends (done) or the flow is
// replaced underneath the reader.
func follow(r *mxl.Reader, cfg mxl.FlowConfig, idx uint64, s *stats, end <-chan time.Time,
	tick <-chan time.Time, stop <-chan os.Signal) (done bool, err error) {
	audio := cfg.Common.Format == mxl.FormatAudio
	data := cfg.Common.Format == mxl.FormatData
	rate := cfg.Common.GrainRate // the sample rate for audio
	step := uint64(1)
	if audio {
		step = batch
	}
	for {
		select {
		case <-stop:
			return true, nil
		case <-end:
			return true, nil
		case <-tick:
			log.Printf("%s", s)
		default:
		}
		var err error
		if audio {
			err = readSamples(r, idx, uint64(cfg.Continuous.ChannelCount), s)
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
			idx = (rt.HeadIndex/step + 1) * step
		case errors.Is(err, mxl.ErrTimeout), errors.Is(err, mxl.ErrOutOfRangeEarly):
			s.timeouts++
		case errors.Is(err, mxl.ErrFlowInvalid):
			return false, nil
		default:
			return true, fmt.Errorf("index %d: %w", idx, err)
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
	p := g.Payload
	if data {
		if g.ValidSlices != anc {
			s.corrupt++
			log.Printf("data grain %d is %d bytes, not %d", idx, g.ValidSlices, anc)
			return nil
		}
		p = p[:anc]
	}
	if o := mismatch(p, idx); o >= 0 {
		s.corrupt++
		if s.corrupt <= 5 {
			log.Printf("grain %d of member %d differs at byte %d of %d", idx, v>>56, o, len(p))
		}
		return nil
	}
	s.ok++
	s.members[v>>56&7]++
	return nil
}

func readSamples(r *mxl.Reader, end, channels uint64, s *stats) error {
	v, err := r.GetSamples(end, batch, 200*time.Millisecond)
	if err != nil {
		return err
	}
	for ch := uint64(0); ch < channels; ch++ {
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
			case x&sampleMark == 0 || x&indexMask != sample(0, ch, idx)&indexMask:
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
