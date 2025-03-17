// Copyright 2018 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

//go:build !codeanalysis
// +build !codeanalysis

// syz-trace2syz converts strace traces to syzkaller programs.
//
// Simple usage:
//
//	strace -o trace -a 1 -s 65500 -v -xx -f -Xraw ./a.out
//	syz-moneta -file trace -image qcow2.image
//
// Intended for seed selection or debugging
package main

import (
	"bufio"
	"flag"
	"io/ioutil"
	"os"
	"path/filepath"
	"runtime"
	"strconv"

	"github.com/google/syzkaller/moneta"
	"github.com/google/syzkaller/pkg/db"
	"github.com/google/syzkaller/pkg/log"
	"github.com/google/syzkaller/pkg/osutil"
	"github.com/google/syzkaller/prog"
	// "github.com/google/syzkaller/sys"
	"github.com/google/syzkaller/tools/syz-trace2syz/proggen"

	"fmt"
	"os/exec"
	"regexp"
	"strings"
	"time"
)

var (
	flagFile        = flag.String("file", "", "file to parse")
	flagDir         = flag.String("dir", "", "directory to parse")
	flagDeserialize = flag.String("deserialize", "", "(Optional) directory to store deserialized programs")
	flagImage       = flag.String("image", "", "qcow2 image")
	flagFd          = flag.String("fd", "", "fdmaps")
	flagBase        = flag.Bool("base", false, "generate baseline")
	flagnofd        = flag.Bool("nofd", false, "generate nofd baseline")
	// limit maximun call nums
	maxCallNum = 200
)

const (
	goos = "linux" // Target OS
)

type Fdmap struct {
	fd    map[uint64]uint64
	isgpu map[uint64]int
}

func main() {
	var arch string
	arch = runtime.GOARCH

	if moneta.MonetaVMArch == "arm64" {
		arch = moneta.MonetaVMArch
	}

	log.Logf(0, "check %v", arch)

	flag.Parse()
	target := initializeTarget(goos, arch)
	progs := parseTraces(target)

	var fdmaps = make([]Fdmap, 0)
	var snapPoint = make([]string, 0)
	if *flagBase {
		baseTime := "00:00:00.000000"
		str := "2014-09-12T" + baseTime + "Z"
		parseTime, _ := time.Parse(time.RFC3339Nano, str)
		point := parseTime.Round(time.Microsecond).UTC().Format("15:04:05.000000")
		fdmaps = append(fdmaps, Fdmap{
			fd:    make(map[uint64]uint64),
			isgpu: make(map[uint64]int),
		})
		snapPoint = append(snapPoint, point)
		maxCallNum = 2000
	} else {
		fdmaps = parseFdmap()
		// Get snapshot point from qcow2 image
		snapPoint = getSnapshotPoint()
	}

	// Combine all the traces into a single trace
	combinedProg := combineToOne(progs, target)

	// Split corpus with Snapshot points
	snapProg := splitWithSnapshotPoint(combinedProg, snapPoint, fdmaps, target)

	log.Logf(0, "successfully converted traces; generating corpus.db")
	pack(snapProg)
}

func initializeTarget(os, arch string) *prog.Target {
	target, err := prog.GetTarget(os, arch)
	if err != nil {
		log.Fatalf("failed to load target: %s", err)
	}
	target.ConstMap = make(map[string]uint64)
	for _, c := range target.Consts {
		target.ConstMap[c.Name] = c.Value
	}
	return target
}

func parseTraces(target *prog.Target) []*prog.Prog {
	var ret []*prog.Prog
	var names []string

	if *flagFile != "" {
		names = append(names, *flagFile)
	} else if *flagDir != "" {
		names = getTraceFiles(*flagDir)
	} else {
		log.Fatalf("-file or -dir must be specified")
	}

	deserializeDir := *flagDeserialize

	totalFiles := len(names)
	log.Logf(0, "parsing %v traces", totalFiles)
	for i, file := range names {
		log.Logf(0, "parsing file %v/%v: %v", i+1, totalFiles, filepath.Base(names[i]))
		progs, err := proggen.ParseFile(file, target)
		if err != nil {
			log.Fatalf("%v", err)
		}
		ret = append(ret, progs...)
		if deserializeDir != "" {
			for i, p := range progs {
				progName := filepath.Join(deserializeDir, filepath.Base(file)+strconv.Itoa(i))
				if err := osutil.WriteFile(progName, p.Serialize()); err != nil {
					log.Fatalf("failed to output file: %v", err)
				}
			}
		}
	}
	return ret
}

func getTraceFiles(dir string) []string {
	infos, err := ioutil.ReadDir(dir)
	if err != nil {
		log.Fatalf("%s", err)

	}
	var names []string
	for _, info := range infos {
		name := filepath.Join(dir, info.Name())
		names = append(names, name)
	}
	return names
}

func parseFdmap() []Fdmap {
	var fdmaps []Fdmap
	var names []string

	if *flagFd != "" {
		names = getFdmaps(*flagFd)
	} else {
		log.Fatalf("-fd (dir) must be specified")
	}
	for _, file := range names {
		fmt.Printf("Fdmap open %v\n", file)
		f, err := os.Open(file)
		if err != nil {
			fmt.Errorf("error reading file: %v", err)
			return nil
		}
		defer f.Close()

		scanner := bufio.NewScanner(f)
		scanner.Split(bufio.ScanWords)

		var fdmap Fdmap
		fdmap.fd = make(map[uint64]uint64)
		fdmap.isgpu = make(map[uint64]int)

		for scanner.Scan() {
			newfd, _ := strconv.Atoi(scanner.Text())
			scanner.Scan()
			oldfd, _ := strconv.Atoi(scanner.Text())
			scanner.Scan()
			gpu := scanner.Text()

			if *flagnofd == false {
				fdmap.fd[uint64(oldfd)] = uint64(newfd)
				if gpu == "n" { // syz_get_snapfd$nvidia
					fdmap.isgpu[uint64(newfd)] = 1
				} else if gpu == "m" { // syz_get_snapfd$mali
					fdmap.isgpu[uint64(newfd)] = 2
				} else if gpu == "a" { // syz_get_snapfd$amdgpu
					fdmap.isgpu[uint64(newfd)] = 3
				} else { // syz_get_snapfd
					fdmap.isgpu[uint64(newfd)] = 0
				}
			} else {
				if gpu == "n" { // syz_get_snapfd$nvidia
					fdmap.isgpu[uint64(newfd)] = 4
					fdmap.fd[uint64(oldfd)] = uint64(newfd)
				} else if gpu == "m" { // syz_get_snapfd$mali
					fdmap.isgpu[uint64(newfd)] = 5
					fdmap.fd[uint64(oldfd)] = uint64(newfd)
				} else if gpu == "a" { // syz_get_snapfd$amdgpu
					fdmap.isgpu[uint64(newfd)] = 6
					fdmap.fd[uint64(oldfd)] = uint64(newfd)
				}
			}
		}

		fdmaps = append(fdmaps, fdmap)

		if err := scanner.Err(); err != nil {
			fmt.Println(err)
		}
	}

	return fdmaps
}

func getFdmaps(dir string) []string {
	fmt.Printf("Open fdmaps dir %v\n", dir)
	infos, err := ioutil.ReadDir(dir)
	if err != nil {
		log.Fatalf("%s", err)
	}
	var names []string
	for _, info := range infos {
		name := filepath.Join(dir, info.Name())
		names = append(names, name)
		fmt.Printf("Open fdmaps %v\n", name)
	}
	return names
}

func pack(progs []*prog.Prog) {
	var records []db.Record
	for _, prog := range progs {
		records = append(records, db.Record{Val: prog.Serialize(), SnapPoint: prog.SnapPoint})
		fmt.Printf("Save snapPoint: %v\n", prog.SnapPoint)
	}
	if err := db.Create("corpus.db", 0, records); err != nil {
		log.Fatalf("%v", err)
	}
	log.Logf(0, "finished!")
}

func combineToOne(progs []*prog.Prog, target *prog.Target) []*prog.Prog {
	var ret []*prog.Prog

	for i, _ := range progs {
		if i == 0 {
			ret = append(ret, progs[i])
		} else {
			ret[0] = combine(ret[0], progs[i], target)
		}
	}

	return ret
}

func getSnapshotPoint() []string {
	var img string
	var snapPoint []string

	if *flagImage != "" {
		img = *flagImage
	}
	fmt.Printf("%v\n", img)
	if _, err := exec.LookPath("qemu-img"); err != nil {
		log.Fatalf("%v", err)
	}
	cmd := exec.Command("qemu-img", "snapshot", "-l", img)

	out, _ := cmd.CombinedOutput()
	outs := strings.Split(string(out), " ")

	// For debugging
	// a := "04:52:30.000000"
	// str := "2014-09-12T" + a + "Z"
	// t, _ := time.Parse(time.RFC3339Nano, str)
	// point := t.Round(time.Microsecond).UTC().Format("15:04:05.000000")
	// snapPoint = append(snapPoint, point)

	// a1 := "04:52:35.000000"
	// str1 := "2014-09-12T" + a1 + "Z"
	// t1, _ := time.Parse(time.RFC3339Nano, str1)
	// point1 := t1.Round(time.Microsecond).UTC().Format("15:04:05.000000")
	// snapPoint = append(snapPoint, point1)

	for _, out := range outs {
		matched, err := regexp.MatchString(`\d{2}:\d{2}:\d{2}.\d{4,9}`, out)
		if err != nil {
			log.Fatalf("%v", err)
		} else if matched {
			str := "2014-09-12T" + out + "Z"
			t, _ := time.Parse(time.RFC3339Nano, str)
			t2 := t.Add(time.Hour * -9)
			point := t2.Round(time.Microsecond).UTC().Format("15:04:05.000000")
			fmt.Printf("%v\n", point)
			snapPoint = append(snapPoint, point)
		}
	}

	for _, k := range snapPoint {
		fmt.Printf("snapPoint: %v\n", k)
	}
	return snapPoint
}

func combine(prog1 *prog.Prog, prog2 *prog.Prog, target *prog.Target) *prog.Prog {
	i := 0
	j := 0
	var c *prog.Call
	c = nil
	var calls []*prog.Call

	for {
		if i < len(prog1.Calls) && j < len(prog2.Calls) {
			ptime := prog1.Calls[i].Begin
			ttime := prog2.Calls[j].Begin
			// fmt.Printf("combine: %v %v\n", ptime, ttime)

			if ptime < ttime {
				c = prog1.Calls[i]
				i++
			} else {
				c = prog2.Calls[j]
				j++
			}
		} else if i < len(prog1.Calls) && j >= len(prog2.Calls) {
			c = prog1.Calls[i]
			i++
		} else if i >= len(prog1.Calls) && j < len(prog2.Calls) {
			c = prog2.Calls[j]
			j++
		} else {
			break
		}
		calls = append(calls, c)
	}

	tmp := prog.MakeProgGen(target)
	for _, c := range calls {
		tmp.Append(c)
	}
	p, err := tmp.Validate()
	if err != nil {
		log.Fatalf("genProg: error validating program: %v", err)
	}
	return p
}

func splitWithSnapshotPoint(progs []*prog.Prog, snapPoint []string, fdmaps []Fdmap, target *prog.Target) []*prog.Prog {
	var ret []*prog.Prog
	var clone []*prog.Prog

	progs[0].SetReturnNum()

	// for i, _ := range fdmaps {
	// 	fmt.Printf("DBG fdmaps %v %v\n", i, len(fdmaps[i].fd))
	// }

	for i := 0; i < len(snapPoint); i++ {
		pBuild := prog.MakeProgGen(target)
		p, err := pBuild.Validate()
		if err != nil {
			log.Fatalf("genProg: error validating program: %v", err)
		}
		clone = append(clone, p)
	}

	var mmapWhole []*prog.Call
	mmap_idx := 0
	for _, call := range progs[0].Calls {
		if call.Meta.Name == "mmap$gpu" {
			mmapWhole = append(mmapWhole, call)
			fmt.Printf("DBG: Insert address: 0x%x\n", call.Args[0].(*prog.PointerArg).TraceAddress)
		}

		if call.Meta.Name == "munmap" {
			for j, mmap := range mmapWhole {
				if mmap.Args[0].(*prog.PointerArg).TraceAddress == call.Args[0].(*prog.PointerArg).TraceAddress {
					mmapWhole = mmapWhole[0:j]
					fmt.Printf("DBG: Remove address: 0x%x\n", call.Args[0].(*prog.PointerArg).TraceAddress)
					break
				}
			}
		}

		if call.Begin > snapPoint[mmap_idx] {
			clone[mmap_idx].MmapInfo = append(clone[mmap_idx].MmapInfo, mmapWhole...)
			mmap_idx += 1
			if mmap_idx == len(snapPoint) {
				break
			}
		}
	}

	idx := 0

	for _, call := range progs[0].Calls {
		if idx+1 < len(snapPoint) {
			if call.Begin > snapPoint[idx+1] {
				idx += 1
			}
		}

		if call.Begin > snapPoint[idx] {
			clone[idx].Calls = append(clone[idx].Calls, call)
		}
	}

	for i, _ := range clone {
		p0c := clone[i].SnapClone(fdmaps[i].fd, fdmaps[i].isgpu)

		for k := len(p0c.Calls) - 1; k >= maxCallNum; k-- {
			p0c.RemoveCall(k)
		}

		p0c.SnapPoint = i + 1
		ret = append(ret, p0c)
	}

	for _, p := range ret {
		for _, c := range p.Calls {
			p.Target.SanitizeCall(c)
		}
	}

	return ret
}
