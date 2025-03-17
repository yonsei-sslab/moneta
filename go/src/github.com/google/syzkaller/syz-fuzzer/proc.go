// Copyright 2017 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package main

import (
	"bytes"
	"fmt"
	"math/rand"
	"os"
	"runtime/debug"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/google/syzkaller/pkg/cover"
	"github.com/google/syzkaller/pkg/hash"
	"github.com/google/syzkaller/pkg/ipc"
	"github.com/google/syzkaller/pkg/log"
	"github.com/google/syzkaller/pkg/rpctype"
	"github.com/google/syzkaller/pkg/signal"
	"github.com/google/syzkaller/prog"
)

const (
	programLength = 150
)

// Proc represents a single fuzzing process (executor).
type Proc struct {
	fuzzer            *Fuzzer
	pid               int
	env               *ipc.Env
	rnd               *rand.Rand
	execOpts          *ipc.ExecOpts
	execOptsCover     *ipc.ExecOpts
	execOptsComps     *ipc.ExecOpts
	execOptsNoCollide *ipc.ExecOpts
	snapPoint         int
}

func newProc(fuzzer *Fuzzer, pid int, env *ipc.Env) (*Proc, error) {
	execOptsNoCollide := *fuzzer.execOpts
	execOptsNoCollide.Flags &= ^ipc.FlagCollide
	execOptsCover := execOptsNoCollide
	execOptsCover.Flags |= ipc.FlagCollectCover
	execOptsComps := execOptsNoCollide
	execOptsComps.Flags |= ipc.FlagCollectComps
	rnd := rand.New(rand.NewSource(time.Now().UnixNano() + int64(pid)*1e12))
	if fuzzer.config.Seed != -1 {
		rnd = rand.New(rand.NewSource(fuzzer.config.Seed))
		log.Logf(2, "Seed: %v", fuzzer.config.Seed)
	}
	proc := &Proc{
		fuzzer:            fuzzer,
		pid:               pid,
		env:               env,
		rnd:               rnd,
		execOpts:          fuzzer.execOpts,
		execOptsCover:     &execOptsCover,
		execOptsComps:     &execOptsComps,
		execOptsNoCollide: &execOptsNoCollide,
		snapPoint:         fuzzer.snapPoint, // 1-indexed
	}
	return proc, nil
}

func (proc *Proc) logMutateStat(op int) {
	switch op {
	case prog.OpSquash:
		proc.fuzzer.stats[StatSquash]++
	case prog.OpSplice:
		proc.fuzzer.stats[StatSplice]++
	case prog.OpInsCall:
		proc.fuzzer.stats[StatInsCall]++
	case prog.OpMutArg:
		proc.fuzzer.stats[StatMutArg]++
	case prog.OpRmCall:
		proc.fuzzer.stats[StatRmCall]++
	}
}

func (proc *Proc) loop() {
	generatePeriod := 100
	if proc.fuzzer.config.Flags&ipc.FlagSignal == 0 {
		// If we don't have real coverage signal, generate programs more frequently
		// because fallback signal is weak.
		generatePeriod = 2
	}
	for i := 0; ; i++ {
		item := proc.fuzzer.workQueue.dequeue()
		if item != nil {
			switch item := item.(type) {
			case *WorkTriage:
				log.Logf(0, "WorkTriage")
				proc.triageInput(item)
			case *WorkCandidate:
				log.Logf(0, "WorkCandidate")
				proc.execute(proc.execOpts, item.p, item.flags, StatCandidate)
			case *WorkSmash:
				log.Logf(0, "WorkSmash")
				proc.smashInput(item)
			default:
				log.Fatalf("unknown work type: %#v", item)
			}
			continue
		}

		ct := proc.fuzzer.choiceTable
		fuzzerSnapshot := proc.fuzzer.snapshot()
		if len(fuzzerSnapshot.corpus) == 0 || i%generatePeriod == 0 {
			// Generate a new prog.

			p := proc.fuzzer.target.GenerateWithFD(proc.rnd, programLength, ct, proc.fuzzer.fdCount)
			p.SnapPoint = proc.snapPoint
			p.FileDescriptorCount = uint64(len(proc.fuzzer.fdCount))
			log.Logf(0, "#%v: generated programLength:%v", proc.pid, programLength)
			execOpts := *proc.execOpts
			execOpts.Flags |= ipc.FlagGenerate
			proc.execute(&execOpts, p, ProgNormal, StatGenerate)
		} else {
			// Mutate an existing prog.
			// mutProg, _ := fuzzerSnapshot.chooseProgram(proc.rnd)
			mutProg, _ := fuzzerSnapshot.snapChooseProgram(proc.rnd, proc.snapPoint)
			if mutProg == nil {
				continue
			}
			p := mutProg.Clone()
			mutOp, mutFromNth := p.Mutate(proc.rnd, programLength, ct, fuzzerSnapshot.corpus)
			proc.logMutateStat(mutOp)
			log.Logf(0, "#%v: mutated (op=%v, from=%v)", proc.pid, mutOp, mutFromNth)
			execOpts := *proc.execOpts
			execOpts.Flags |= ipc.FlagFuzz
			execOpts.FaultNth = mutFromNth // re-purposed a field unused by Agamotto
			proc.execute(&execOpts, p, ProgNormal, StatFuzz)
		}
	}
}

func (proc *Proc) triageInput(item *WorkTriage) {
	log.Logf(1, "#%v: triaging type=%x", proc.pid, item.flags)

	prio := signalPrio(item.p, &item.info, item.call)
	inputSignal := signal.FromRaw(item.info.Signal, prio)
	newSignal := proc.fuzzer.corpusSignalDiff(inputSignal)
	if newSignal.Empty() {
		return
	}
	callName := ".extra"
	logCallName := "extra"
	if item.call != -1 {
		callName = item.p.Calls[item.call].Meta.Name
		logCallName = fmt.Sprintf("call #%v %v", item.call, callName)
	}
	log.Logf(0, "triaging input for %v (new signal=%v) snappoint=%v", logCallName, newSignal.Len(), item.p.SnapPoint)
	var inputCover cover.Cover
	const (
		signalRuns       = 3
		minimizeAttempts = 3
	)
	// Compute input coverage and non-flaky signal for minimization.
	notexecuted := 0
	for i := 0; i < signalRuns; i++ {
		execOpts := *proc.execOptsCover
		execOpts.Flags |= ipc.FlagTriage
		info := proc.executeRaw(&execOpts, item.p, StatTriage)
		if !reexecutionSuccess(info, &item.info, item.call) {
			// The call was not executed or failed.
			notexecuted++
			if notexecuted > signalRuns/2+1 {
				return // if happens too often, give up
			}
			continue
		}
		thisSignal, thisCover := getSignalAndCover(item.p, info, item.call)
		newSignal = newSignal.Intersection(thisSignal)
		// Without !minimized check manager starts losing some considerable amount
		// of coverage after each restart. Mechanics of this are not completely clear.
		if newSignal.Empty() && item.flags&ProgMinimized == 0 {
			return
		}
		inputCover.Merge(thisCover)
	}
	if item.flags&ProgMinimized == 0 {
		item.p, item.call = prog.Minimize(item.p, item.call, false,
			func(p1 *prog.Prog, call1 int) bool {
				execOpts := *proc.execOptsNoCollide
				execOpts.Flags |= ipc.FlagMinimize
				for i := 0; i < minimizeAttempts; i++ {
					info := proc.execute(&execOpts, p1, ProgNormal, StatMinimize)
					if !reexecutionSuccess(info, &item.info, call1) {
						if i == minimizeAttempts-1 {
							atomic.AddUint64(&proc.fuzzer.stats[StatMinimizeN], 1)
						} else {
							atomic.AddUint64(&proc.fuzzer.stats[StatMinimizeC], 1)
						}
						execOpts.Flags |= ipc.FlagMinimizeRetry
						// The call was not executed or failed.
						continue
					}
					thisSignal, _ := getSignalAndCover(p1, info, call1)
					if newSignal.Intersection(thisSignal).Len() == newSignal.Len() {
						atomic.AddUint64(&proc.fuzzer.stats[StatMinimizeS], 1)
						return true
					}
					if i == minimizeAttempts-1 {
						atomic.AddUint64(&proc.fuzzer.stats[StatMinimizeN], 1)
					} else {
						atomic.AddUint64(&proc.fuzzer.stats[StatMinimizeR], 1)
					}
					execOpts.Flags |= ipc.FlagMinimizeRetry
				}
				return false
			})
	}

	data := item.p.Serialize()
	sig := hash.Hash(data)

	log.Logf(2, "added new input for %v to corpus:\n%s", logCallName, data)

	proc.fuzzer.sendInputToManager(rpctype.RPCInput{
		Call:      callName,
		Prog:      data,
		Signal:    inputSignal.Serialize(),
		Cover:     inputCover.Serialize(),
		SnapPoint: item.p.SnapPoint,
	})

	proc.fuzzer.addInputToCorpus(item.p, inputSignal, sig)

	if item.flags&ProgSmashed == 0 {
		proc.fuzzer.workQueue.enqueue(&WorkSmash{item.p, item.call})
	}
}

func reexecutionSuccess(info *ipc.ProgInfo, oldInfo *ipc.CallInfo, call int) bool {
	if info == nil || len(info.Calls) == 0 {
		return false
	}
	if call != -1 {
		// Don't minimize calls from successful to unsuccessful.
		// Successful calls are much more valuable.
		if oldInfo.Errno == 0 && info.Calls[call].Errno != 0 {
			return false
		}
		return len(info.Calls[call].Signal) != 0
	}
	return len(info.Extra.Signal) != 0
}

func getSignalAndCover(p *prog.Prog, info *ipc.ProgInfo, call int) (signal.Signal, []uint32) {
	inf := &info.Extra
	if call != -1 {
		inf = &info.Calls[call]
	}
	return signal.FromRaw(inf.Signal, signalPrio(p, inf, call)), inf.Cover
}

func (proc *Proc) smashInput(item *WorkSmash) {
	if proc.fuzzer.faultInjectionEnabled && item.call != -1 {
		proc.failCall(item.p, item.call)
	}
	if proc.fuzzer.comparisonTracingEnabled && item.call != -1 {
		proc.executeHintSeed(item.p, item.call)
	}
	fuzzerSnapshot := proc.fuzzer.snapshot()
	for i := 0; i < 100; i++ {
		p := item.p.Clone()
		mutOp, mutFromNth := p.Mutate(proc.rnd, programLength, proc.fuzzer.choiceTable, fuzzerSnapshot.corpus)
		proc.logMutateStat(mutOp)
		log.Logf(1, "#%v: smash mutated (op=%v, from=%v)", proc.pid, mutOp, mutFromNth)
		execOpts := *proc.execOpts
		execOpts.Flags |= ipc.FlagSmash
		execOpts.FaultNth = mutFromNth
		proc.execute(&execOpts, p, ProgNormal, StatSmash)
	}
}

func (proc *Proc) failCall(p *prog.Prog, call int) {
	for nth := 0; nth < 100; nth++ {
		log.Logf(1, "#%v: injecting fault into call %v/%v", proc.pid, call, nth)
		opts := *proc.execOpts
		opts.Flags |= ipc.FlagInjectFault
		opts.FaultCall = call
		opts.FaultNth = nth
		info := proc.executeRaw(&opts, p, StatSmash)
		if info != nil && len(info.Calls) > call && info.Calls[call].Flags&ipc.CallFaultInjected == 0 {
			break
		}
	}
}

func (proc *Proc) executeHintSeed(p *prog.Prog, call int) {
	log.Logf(1, "#%v: collecting comparisons", proc.pid)
	// First execute the original program to dump comparisons from KCOV.
	info := proc.execute(proc.execOptsComps, p, ProgNormal, StatSeed)
	if info == nil {
		return
	}

	// Then mutate the initial program for every match between
	// a syscall argument and a comparison operand.
	// Execute each of such mutants to check if it gives new coverage.
	p.MutateWithHints(call, info.Calls[call].Comps, func(p *prog.Prog) {
		log.Logf(1, "#%v: executing comparison hint", proc.pid)
		proc.execute(proc.execOpts, p, ProgNormal, StatHint)
	})
}

func (proc *Proc) execute(execOpts *ipc.ExecOpts, p *prog.Prog, flags ProgTypes, stat Stat) *ipc.ProgInfo {
	info := proc.executeRaw(execOpts, p, stat)
	if info.Crashed {
		// let's not consider adding crashing input to corpus
		// and update rng as it has to be updated in the baseline
		if proc.fuzzer.config.Seed == -1 {
			proc.rnd = rand.New(rand.NewSource(time.Now().UnixNano() + int64(proc.pid)*1e12))
		} else {
			proc.fuzzer.config.Seed += 128
			proc.rnd = rand.New(rand.NewSource(proc.fuzzer.config.Seed))
		}
		return info
	}
	calls, extra := proc.fuzzer.checkNewSignal(p, info)
	for _, callIndex := range calls {
		proc.enqueueCallTriage(p, flags, callIndex, info.Calls[callIndex])
	}
	if extra {
		proc.enqueueCallTriage(p, flags, -1, info.Extra)
	}
	return info
}

func (proc *Proc) enqueueCallTriage(p *prog.Prog, flags ProgTypes, callIndex int, info ipc.CallInfo) {
	// info.Signal points to the output shmem region, detach it before queueing.
	info.Signal = append([]uint32{}, info.Signal...)
	// None of the caller use Cover, so just nil it instead of detaching.
	// Note: triage input uses executeRaw to get coverage.
	info.Cover = nil
	proc.fuzzer.workQueue.enqueue(&WorkTriage{
		p:     p.Clone(),
		call:  callIndex,
		info:  info,
		flags: flags,
	})
}

func (proc *Proc) executeRaw(opts *ipc.ExecOpts, p *prog.Prog, stat Stat) *ipc.ProgInfo {
	if opts.Flags&ipc.FlagDedupCover == 0 {
		log.Fatalf("dedup cover is not enabled")
	}

	// Limit concurrency window and do leak checking once in a while.
	ticket := proc.fuzzer.gate.Enter()
	defer proc.fuzzer.gate.Leave(ticket)

	proc.logProgram(opts, p)
	for try := 0; ; try++ {
		atomic.AddUint64(&proc.fuzzer.stats[stat], 1)
		output, info, hanged, err := proc.env.Exec(opts, p)
		if info != nil {
			for stat := ipc.Stat(0); stat < ipc.StatCount; stat++ {
				atomic.AddUint64(&proc.fuzzer.statsAgamotto[stat], uint64(info.Stats[stat]))
			}
			for stat := ipc.Stat(0); stat < ipc.TStatCount; stat++ {
				v := uint64(info.TimeStats[stat])
				for bucket := ipc.Stat(0); bucket < ipc.TBucketCount; bucket++ {
					if v <= ipc.TBucketRanges[bucket] {
						atomic.AddUint64(&proc.fuzzer.statsAgamotto[ipc.StatCount+stat*ipc.TBucketCount+bucket], 1)
						break
					}
				}
			}
			for stat := ipc.Stat(0); stat < ipc.DPStatCount; stat++ {
				v := uint64(info.DirtyPageStats[stat])
				for bucket := ipc.Stat(0); bucket < ipc.DPBucketCount; bucket++ {
					if v <= ipc.DPBucketRanges[bucket] {
						atomic.AddUint64(&proc.fuzzer.statsAgamotto[ipc.StatCount+ipc.TStatCount*ipc.TBucketCount+stat*ipc.DPBucketCount+bucket], 1)
						break
					}
				}
			}
			for stat := ipc.Stat(0); stat < ipc.CPStatCount; stat++ {
				for _, item := range info.ChkptStats[stat] {
					v := uint64(item)
					for bucket := ipc.Stat(0); bucket < ipc.CPBucketCount; bucket++ {
						if v <= ipc.CPBucketRanges[bucket] {
							atomic.AddUint64(&proc.fuzzer.statsAgamotto[ipc.StatCount+ipc.TStatCount*ipc.TBucketCount+ipc.DPStatCount*ipc.DPBucketCount+stat*ipc.CPBucketCount+bucket], 1)
							break
						}
					}
				}
			}
		}
		if err != nil {
			if stat == StatMinimize {
				atomic.AddUint64(&proc.fuzzer.stats[StatMinimizeF], 1)
			}
			if try > 10 {
				log.Fatalf("executor %v failed %v times:\n%v", proc.pid, try, err)
			}
			log.Logf(0, "fuzzer detected executor failure='%v', retrying #%d", err, try+1)
			debug.FreeOSMemory()
			time.Sleep(time.Second)
			continue
		}
		log.Logf(2, "result hanged=%v: %s", hanged, output)
		return info
	}
}

func (proc *Proc) logProgram(opts *ipc.ExecOpts, p *prog.Prog) {
	if proc.fuzzer.outputType == OutputNone {
		return
	}

	data := p.Serialize()
	strOpts := ""
	if opts.Flags&ipc.FlagInjectFault != 0 {
		strOpts = fmt.Sprintf(" (fault-call:%v fault-nth:%v)", opts.FaultCall, opts.FaultNth)
	}

	// The following output helps to understand what program crashed kernel.
	// It must not be intermixed.
	switch proc.fuzzer.outputType {
	case OutputStdout:
		now := time.Now()
		proc.fuzzer.logMu.Lock()
		fmt.Printf("%02v:%02v:%02v executing program %v%v:\n%s\n",
			now.Hour(), now.Minute(), now.Second(),
			proc.pid, strOpts, data)
		proc.fuzzer.logMu.Unlock()
	case OutputDmesg:
		fd, err := syscall.Open("/dev/kmsg", syscall.O_WRONLY, 0)
		if err == nil {
			buf := new(bytes.Buffer)
			fmt.Fprintf(buf, "syzkaller: executing program %v%v:\n%s\n",
				proc.pid, strOpts, data)
			syscall.Write(fd, buf.Bytes())
			syscall.Close(fd)
		}
	case OutputFile:
		f, err := os.Create(fmt.Sprintf("%v-%v.prog", proc.fuzzer.name, proc.pid))
		if err == nil {
			if strOpts != "" {
				fmt.Fprintf(f, "#%v\n", strOpts)
			}
			f.Write(data)
			f.Close()
		}
	default:
		log.Fatalf("unknown output type: %v", proc.fuzzer.outputType)
	}
}
