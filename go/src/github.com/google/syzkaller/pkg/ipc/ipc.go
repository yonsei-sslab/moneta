// Copyright 2015 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package ipc

import (
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync/atomic"
	"time"
	"unsafe"

	"github.com/google/syzkaller/pkg/cover"
	"github.com/google/syzkaller/pkg/log"
	"github.com/google/syzkaller/pkg/osutil"
	"github.com/google/syzkaller/pkg/signal"
	"github.com/google/syzkaller/prog"
	"github.com/google/syzkaller/sys/targets"
)

// Configuration flags for Config.Flags.
type EnvFlags uint64

// Note: New / changed flags should be added to parse_env_flags in executor.cc
const (
	FlagDebug            EnvFlags = 1 << iota // debug output from executor
	FlagSignal                                // collect feedback signals (coverage)
	FlagSandboxSetuid                         // impersonate nobody user
	FlagSandboxNamespace                      // use namespaces for sandboxing
	FlagSandboxAndroid                        // use Android sandboxing for the untrusted_app domain
	FlagExtraCover                            // collect extra coverage
	FlagEnableTun                             // setup and use /dev/tun for packet injection
	FlagEnableNetDev                          // setup more network devices for testing
	FlagEnableNetReset                        // reset network namespace between programs
	FlagEnableCgroups                         // setup cgroups for testing
	FlagEnableCloseFds                        // close fds after each program
	FlagEnableDevlinkPCI                      // setup devlink PCI device

	// PERISCOPE
	FlagVirtCover
	FlagRootChkptOnly

	// MONETA
	FlagExecveTracee
)

// Per-exec flags for ExecOpts.Flags:
type ExecFlags uint64

const (
	FlagCollectCover ExecFlags = 1 << iota // collect coverage
	FlagDedupCover                         // deduplicate coverage in executor
	FlagInjectFault                        // inject a fault in this execution (see ExecOpts)
	FlagCollectComps                       // collect KCOV comparisons
	FlagThreaded                           // use multiple threads to mitigate blocked syscalls
	FlagCollide                            // collide syscalls to provoke data races

	// PeriScope chkpt & restore hints
	FlagTriage
	FlagMinimize
	FlagMinimizeRetry
	FlagFuzz
	FlagGenerate
	FlagSmash
)

type ExecOpts struct {
	Flags     ExecFlags
	FaultCall int // call index for fault injection (0-based)
	FaultNth  int // fault n-th operation in the call (0-based)
}

// Config is the configuration for Env.
type Config struct {
	// Path to executor binary.
	Executor string

	UseShmem      bool // use shared memory instead of pipes for communication
	UseVirtShmem  bool // PeriScope
	UseForkServer bool // use extended protocol with handshake

	// Flags are configuation flags, defined above.
	Flags EnvFlags

	Index int
	// PeriScope (fuzzer <-> VMM)
	CtlPipe *os.File
	StPipe  *os.File
	// PeriScope (fuzzer <-> executor)
	InPipe  *os.File
	OutPipe *os.File
	ErrPipe *os.File

	// Timeout is the execution timeout for a single program.
	Timeout time.Duration

	// Seed
	Seed int64
}

type CallFlags uint32

const (
	CallExecuted      CallFlags = 1 << iota // was started at all
	CallFinished                            // finished executing (rather than blocked forever)
	CallBlocked                             // finished but blocked during execution
	CallFaultInjected                       // fault was injected into this call
)

type CallInfo struct {
	Flags  CallFlags
	Signal []uint32 // feedback signal, filled if FlagSignal is set
	Cover  []uint32 // per-call coverage, filled if FlagSignal is set and cover == true,
	// if dedup == false, then cov effectively contains a trace, otherwise duplicates are removed
	Comps prog.CompMap // per-call comparison operands
	Errno int          // call errno (0 if the call was successful)
}

type Stat int

const (
	StatTimeSaved Stat = iota
	StatTimeExec
	StatNonRootRestores
	StatChkptsCreated
	StatKilled
	StatEvictPolicy1
	StatEvictPolicy2
	StatEvictPolicy3
	StatEvictPolicy4
	StatCount
)

var StatNames = [StatCount]string{
	StatTimeSaved:       "agamotto: time skip (s)",
	StatTimeExec:        "agamotto: time exec (s)",
	StatNonRootRestores: "agamotto: non-root restores",
	StatChkptsCreated:   "agamotto: non-root chkpts",
	StatKilled:          "agamotto: killed",
	StatEvictPolicy1:    "agamotto: evict policy 1",
	StatEvictPolicy2:    "agamotto: evict policy 2",
	StatEvictPolicy3:    "agamotto: evict policy 3",
	StatEvictPolicy4:    "agamotto: evict policy 4",
}

const (
	TStatExec Stat = iota
	TStatSkip
	TStatActual
	TStatCount
)

var TStatNames = [TStatCount]string{
	TStatExec:   "agamotto: exec time %v",
	TStatSkip:   "agamotto: skip time %v",
	TStatActual: "agamotto: actual time %v",
}

const (
	TBucket0ms Stat = iota
	TBucket250ms
	TBucket500ms
	TBucket750ms
	TBucket1000ms
	TBucket1250ms
	TBucket1500ms
	TBucket1750ms
	TBucket2000ms
	TBucket2250ms
	TBucket2500ms
	TBucket2750ms
	TBucket3000ms
	TBucket3250ms
	TBucket3500ms
	TBucket3750ms
	TBucket4000ms
	TBucket4250ms
	TBucket4500ms
	TBucket4750ms
	TBucket5000ms
	TBucket5250ms
	TBucket5500ms
	TBucket5750ms
	TBucket6000ms
	TBucket6250ms
	TBucket6500ms
	TBucket6750ms
	TBucket7000ms
	TBucket7250ms
	TBucket7500ms
	TBucket7750ms
	TBucket8000ms
	TBucket8250ms
	TBucket8500ms
	TBucket8750ms
	TBucket9000ms
	TBucketRest
	TBucketCount
)

var TBucketRanges = [TBucketCount]uint64{
	TBucket0ms:    0,
	TBucket250ms:  250,
	TBucket500ms:  500,
	TBucket750ms:  750,
	TBucket1000ms: 1000,
	TBucket1250ms: 1250,
	TBucket1500ms: 1500,
	TBucket1750ms: 1750,
	TBucket2000ms: 2000,
	TBucket2250ms: 2250,
	TBucket2500ms: 2500,
	TBucket2750ms: 2750,
	TBucket3000ms: 3000,
	TBucket3250ms: 3250,
	TBucket3500ms: 3500,
	TBucket3750ms: 3750,
	TBucket4000ms: 4000,
	TBucket4250ms: 4250,
	TBucket4500ms: 4500,
	TBucket4750ms: 4750,
	TBucket5000ms: 5000,
	TBucket5250ms: 5250,
	TBucket5500ms: 5500,
	TBucket5750ms: 5750,
	TBucket6000ms: 6000,
	TBucket6250ms: 6250,
	TBucket6500ms: 6500,
	TBucket6750ms: 6750,
	TBucket7000ms: 7000,
	TBucket7250ms: 7250,
	TBucket7500ms: 7500,
	TBucket7750ms: 7750,
	TBucket8000ms: 8000,
	TBucket8250ms: 8250,
	TBucket8500ms: 8500,
	TBucket8750ms: 8750,
	TBucket9000ms: 9000,
	TBucketRest:   ^uint64(0),
}

const (
	DPStatRestored Stat = iota
	DPStatDirtied
	DPStatCount
)

var DPStatNames = [DPStatCount]string{
	DPStatRestored: "agamotto: pgs restored %v",
	DPStatDirtied:  "agamotto: pgs dirtied %v",
}

const (
	//DPBucket500 Stat = iota
	DPBucket1K Stat = iota
	DPBucket2K
	DPBucket3K
	DPBucket4K
	DPBucket5K
	DPBucket6K
	DPBucket7K
	DPBucket8K
	DPBucket9K
	DPBucket10K
	DPBucket11K
	DPBucket12K
	DPBucket13K
	DPBucket14K
	DPBucket15K
	//DPBucket16K
	DPBucketRest
	DPBucketCount
)

var DPBucketRanges = [DPBucketCount]uint64{
	DPBucket1K:  1000,
	DPBucket2K:  2000,
	DPBucket3K:  3000,
	DPBucket4K:  4000,
	DPBucket5K:  5000,
	DPBucket6K:  6000,
	DPBucket7K:  7000,
	DPBucket8K:  8000,
	DPBucket9K:  9000,
	DPBucket10K: 10000,
	DPBucket11K: 11000,
	DPBucket12K: 12000,
	DPBucket13K: 13000,
	DPBucket14K: 14000,
	DPBucket15K: 15000,
	//DPBucket16K:  16000,
	DPBucketRest: ^uint64(0),
}

const (
	CPStatSize Stat = iota
	CPStatCount
)

var CPStatNames = [CPStatCount]string{
	CPStatSize: "agamotto: chkpt sz %vMiB",
}

const (
	CPItem0 Stat = iota
	CPItem1
	CPItem2
	CPItem3
	CPItem4
	CPItem5
	CPItem6
	CPItem7
	CPItem8
	CPItem9
	CPItemCount
)

const (
	CPBucket2M Stat = iota
	CPBucket4M
	CPBucket6M
	CPBucket8M
	CPBucket10M
	CPBucket12M
	CPBucket14M
	CPBucket16M
	CPBucket18M
	CPBucket20M
	CPBucket22M
	CPBucket24M
	CPBucket26M
	CPBucket28M
	CPBucket30M
	CPBucket32M
	CPBucket34M
	CPBucket36M
	CPBucket38M
	CPBucket40M
	CPBucket42M
	CPBucket44M
	CPBucket46M
	CPBucket48M
	CPBucketRest
	CPBucketCount
)

var CPBucketRanges = [CPBucketCount]uint64{
	CPBucket2M:   1024 * 2,
	CPBucket4M:   1024 * 4,
	CPBucket6M:   1024 * 6,
	CPBucket8M:   1024 * 8,
	CPBucket10M:  1024 * 10,
	CPBucket12M:  1024 * 12,
	CPBucket14M:  1024 * 14,
	CPBucket16M:  1024 * 16,
	CPBucket18M:  1024 * 18,
	CPBucket20M:  1024 * 20,
	CPBucket22M:  1024 * 22,
	CPBucket24M:  1024 * 24,
	CPBucket26M:  1024 * 26,
	CPBucket28M:  1024 * 28,
	CPBucket30M:  1024 * 30,
	CPBucket32M:  1024 * 32,
	CPBucket34M:  1024 * 34,
	CPBucket36M:  1024 * 36,
	CPBucket38M:  1024 * 38,
	CPBucket40M:  1024 * 40,
	CPBucket42M:  1024 * 42,
	CPBucket44M:  1024 * 44,
	CPBucket46M:  1024 * 46,
	CPBucket48M:  1024 * 48,
	CPBucketRest: ^uint64(0),
}

type ProgInfo struct {
	Calls []CallInfo
	Extra CallInfo // stores Signal and Cover collected from background threads

	Crashed        bool
	Stats          [StatCount]uint32
	TimeStats      [TStatCount]uint32
	DirtyPageStats [DPStatCount]uint32
	ChkptStats     [CPStatCount][]uint32
}

type Env struct {
	in  []byte
	out []byte

	// PeriScope
	stPipe  *os.File
	ctlPipe *os.File

	cmd       *command
	inFile    *os.File
	outFile   *os.File
	bin       []string
	linkedBin string
	pid       int
	config    *Config

	StatExecs    uint64
	StatRestarts uint64
}

const (
	outputSize = 16 << 20

	statusFail = 67

	// Comparison types masks taken from KCOV headers.
	compSizeMask  = 6
	compSize8     = 6
	compConstMask = 1

	extraReplyIndex = 0xffffffff // uint32(-1)
)

func SandboxToFlags(sandbox string) (EnvFlags, error) {
	switch sandbox {
	case "none":
		return 0, nil
	case "setuid":
		return FlagSandboxSetuid, nil
	case "namespace":
		return FlagSandboxNamespace, nil
	case "android":
		return FlagSandboxAndroid, nil
	default:
		return 0, fmt.Errorf("sandbox must contain one of none/setuid/namespace/android")
	}
}

func FlagsToSandbox(flags EnvFlags) string {
	if flags&FlagSandboxSetuid != 0 {
		return "setuid"
	} else if flags&FlagSandboxNamespace != 0 {
		return "namespace"
	} else if flags&FlagSandboxAndroid != 0 {
		return "android"
	}
	return "none"
}

func MakeEnv(config *Config, pid int) (*Env, error) {
	var inf, outf *os.File
	var inmem, outmem []byte
	if config.UseShmem && config.UseVirtShmem {
		var err error
		inf, inmem, err = osutil.OpenVirtMemFile(
			fmt.Sprintf("/dev/shm/syzkaller-vm%v-in", config.Index), prog.ExecBufferSize)
		if err != nil {
			return nil, err
		}
		outf, outmem, err = osutil.OpenVirtMemFile(
			fmt.Sprintf("/dev/shm/syzkaller-vm%v-out", config.Index), outputSize)
		if err != nil {
			return nil, err
		}
	} else if config.UseShmem {
		var err error
		inf, inmem, err = osutil.CreateMemMappedFile(prog.ExecBufferSize)
		if err != nil {
			return nil, err
		}
		defer func() {
			if inf != nil {
				osutil.CloseMemMappedFile(inf, inmem)
			}
		}()
		outf, outmem, err = osutil.CreateMemMappedFile(outputSize)
		if err != nil {
			return nil, err
		}
		defer func() {
			if outf != nil {
				osutil.CloseMemMappedFile(outf, outmem)
			}
		}()
	} else {
		inmem = make([]byte, prog.ExecBufferSize)
		outmem = make([]byte, outputSize)
	}
	env := &Env{
		in:      inmem,
		out:     outmem,
		inFile:  inf,
		outFile: outf,
		ctlPipe: config.CtlPipe,
		stPipe:  config.StPipe,
		bin:     strings.Split(config.Executor, " "),
		pid:     pid,
		config:  config,
	}
	if len(env.bin) == 0 {
		return nil, fmt.Errorf("binary is empty string")
	}
	env.bin[0] = osutil.Abs(env.bin[0]) // we are going to chdir
	// Append pid to binary name.
	// E.g. if binary is 'syz-executor' and pid=15,
	// we create a link from 'syz-executor.15' to 'syz-executor' and use 'syz-executor.15' as binary.
	// This allows to easily identify program that lead to a crash in the log.
	// Log contains pid in "executing program 15" and crashes usually contain "Comm: syz-executor.15".
	// Note: pkg/report knowns about this and converts "syz-executor.15" back to "syz-executor".
	base := filepath.Base(env.bin[0])
	pidStr := fmt.Sprintf(".%v", pid)
	const maxLen = 16 // TASK_COMM_LEN is currently set to 16
	if len(base)+len(pidStr) >= maxLen {
		// Remove beginning of file name, in tests temp files have unique numbers at the end.
		base = base[len(base)+len(pidStr)-maxLen+1:]
	}
	binCopy := filepath.Join(filepath.Dir(env.bin[0]), base+pidStr)
	if err := os.Link(env.bin[0], binCopy); err == nil {
		env.bin[0] = binCopy
		env.linkedBin = binCopy
	}
	inf = nil
	outf = nil
	return env, nil
}

func (env *Env) Close() error {
	if env.cmd != nil {
		env.cmd.close()
	}
	if env.linkedBin != "" {
		os.Remove(env.linkedBin)
	}
	var err1, err2 error
	if env.inFile != nil {
		err1 = osutil.CloseMemMappedFile(env.inFile, env.in)
	}
	if env.outFile != nil {
		err2 = osutil.CloseMemMappedFile(env.outFile, env.out)
	}
	switch {
	case err1 != nil:
		return err1
	case err2 != nil:
		return err2
	default:
		return nil
	}
}

var rateLimit = time.NewTicker(1 * time.Second)

// Exec starts executor binary to execute program p and returns information about the execution:
// output: process output
// info: per-call info
// hanged: program hanged and was killed
// err0: failed to start the process or bug in executor itself
func (env *Env) Exec(opts *ExecOpts, p *prog.Prog) (output []byte, info *ProgInfo, hanged bool, err0 error) {
	// Copy-in serialized program.
	progSize, err := p.SerializeForExec(env.in)
	if err != nil {
		err0 = fmt.Errorf("failed to serialize: %v", err)
		return
	}
	var progData []byte
	if !env.config.UseShmem {
		progData = env.in[:progSize]
	}
	// Zero out the first two words (ncmd and nsig), so that we don't have garbage there
	// if executor crashes before writing non-garbage there.
	for i := 0; i < 4; i++ {
		env.out[i] = 0
	}

	atomic.AddUint64(&env.StatExecs, 1)
	if env.cmd == nil {
		if p.Target.OS != "test" && targets.Get(p.Target.OS, p.Target.Arch).HostFuzzer {
			// The executor is actually ssh,
			// starting them too frequently leads to timeouts.
			<-rateLimit.C
		}
		tmpDirPath := "./"
		if env.config.InPipe != nil && env.config.OutPipe != nil && env.config.ErrPipe != nil {
			log.Logf(0, "executor already running - using existing in/out/err pipes")
			env.cmd, err0 = makeCommandWithPipes(env.pid, env.bin, env.config, env.inFile, env.outFile, env.stPipe, env.ctlPipe, env.in, env.out, tmpDirPath)
		} else {
			atomic.AddUint64(&env.StatRestarts, 1)
			env.cmd, err0 = makeCommand(env.pid, env.bin, env.config, env.inFile, env.outFile, env.in, env.out, tmpDirPath)
		}
		if err0 != nil {
			return
		}
	}
	var crashed bool
	var stats [StatCount + TStatCount + DPStatCount + CPStatCount*CPItemCount]uint32
	output, stats, hanged, crashed, err0 = env.cmd.exec(opts, progData)
	if err0 != nil {
		if env.config.InPipe != nil && env.config.OutPipe != nil && env.config.ErrPipe != nil {
			return
		}
		env.cmd.close()
		env.cmd = nil
		return
	}
	info, err0 = env.parseOutput(p)
	if info != nil {
		info.Crashed = crashed
		for stat := Stat(0); stat < StatCount; stat++ {
			info.Stats[stat] = stats[stat]
		}
		for stat := Stat(0); stat < TStatCount; stat++ {
			info.TimeStats[stat] = stats[StatCount+stat]
		}
		for stat := Stat(0); stat < DPStatCount; stat++ {
			info.DirtyPageStats[stat] = stats[StatCount+TStatCount+stat]
		}
		for stat := Stat(0); stat < CPStatCount; stat++ {
			chkptStats := make([]uint32, 0)
			for item := Stat(0); item < CPItemCount; item++ {
				v := stats[StatCount+TStatCount+DPStatCount+stat*CPItemCount+item]
				if v == 0 {
					break
				}
				chkptStats = append(chkptStats, v)
			}
			info.ChkptStats[stat] = chkptStats
		}
	}
	if info != nil && env.config.Flags&FlagSignal == 0 {
		addFallbackSignal(p, info)
	}
	if !env.config.UseForkServer {
		env.cmd.close()
		env.cmd = nil
	}
	return
}

// addFallbackSignal computes simple fallback signal in cases we don't have real coverage signal.
// We use syscall number or-ed with returned errno value as signal.
// At least this gives us all combinations of syscall+errno.
func addFallbackSignal(p *prog.Prog, info *ProgInfo) {
	log.Logf(0, "adding fallback signal")
	callInfos := make([]prog.CallInfo, len(info.Calls))
	for i, inf := range info.Calls {
		if inf.Flags&CallExecuted != 0 {
			callInfos[i].Flags |= prog.CallExecuted
		}
		if inf.Flags&CallFinished != 0 {
			callInfos[i].Flags |= prog.CallFinished
		}
		if inf.Flags&CallBlocked != 0 {
			callInfos[i].Flags |= prog.CallBlocked
		}
		callInfos[i].Errno = inf.Errno
	}
	p.FallbackSignal(callInfos)
	for i, inf := range callInfos {
		info.Calls[i].Signal = inf.Signal
	}
}

func (env *Env) parseOutput(p *prog.Prog) (*ProgInfo, error) {
	out := env.out
	ncmd, ok := readUint32(&out)
	if !ok {
		return nil, fmt.Errorf("failed to read number of calls")
	}
	info := &ProgInfo{Calls: make([]CallInfo, len(p.Calls))}
	extraParts := make([]CallInfo, 0)
	for i := uint32(0); i < ncmd; i++ {
		if len(out) < int(unsafe.Sizeof(callReply{})) {
			return nil, fmt.Errorf("failed to read call %v reply", i)
		}
		reply := *(*callReply)(unsafe.Pointer(&out[0]))
		out = out[unsafe.Sizeof(callReply{}):]
		var inf *CallInfo
		if reply.index != extraReplyIndex {
			if int(reply.index) >= len(info.Calls) {
				return nil, fmt.Errorf("bad call %v index %v/%v", i, reply.index, len(info.Calls))
			}
			if num := p.Calls[reply.index].Meta.ID; int(reply.num) != num {
				return nil, fmt.Errorf("wrong call %v num %v/%v", i, reply.num, num)
			}
			inf = &info.Calls[reply.index]
			if inf.Flags != 0 || inf.Signal != nil {
				return nil, fmt.Errorf("duplicate reply for call %v/%v/%v", i, reply.index, reply.num)
			}
			inf.Errno = int(reply.errno)
			inf.Flags = CallFlags(reply.flags)
		} else {
			extraParts = append(extraParts, CallInfo{})
			inf = &extraParts[len(extraParts)-1]
		}
		if inf.Signal, ok = readUint32Array(&out, reply.signalSize); !ok {
			return nil, fmt.Errorf("call %v/%v/%v: signal overflow: %v/%v",
				i, reply.index, reply.num, reply.signalSize, len(out))
		}
		if inf.Cover, ok = readUint32Array(&out, reply.coverSize); !ok {
			return nil, fmt.Errorf("call %v/%v/%v: cover overflow: %v/%v",
				i, reply.index, reply.num, reply.coverSize, len(out))
		}
		comps, err := readComps(&out, reply.compsSize)
		if err != nil {
			return nil, err
		}
		inf.Comps = comps
	}
	if len(extraParts) == 0 {
		return info, nil
	}
	info.Extra = convertExtra(extraParts)
	return info, nil
}

func convertExtra(extraParts []CallInfo) CallInfo {
	var extra CallInfo
	extraCover := make(cover.Cover)
	extraSignal := make(signal.Signal)
	for _, part := range extraParts {
		extraCover.Merge(part.Cover)
		extraSignal.Merge(signal.FromRaw(part.Signal, 0))
	}
	extra.Cover = extraCover.Serialize()
	extra.Signal = make([]uint32, len(extraSignal))
	i := 0
	for s := range extraSignal {
		extra.Signal[i] = uint32(s)
		i++
	}
	return extra
}

func readComps(outp *[]byte, compsSize uint32) (prog.CompMap, error) {
	if compsSize == 0 {
		return nil, nil
	}
	compMap := make(prog.CompMap)
	for i := uint32(0); i < compsSize; i++ {
		typ, ok := readUint32(outp)
		if !ok {
			return nil, fmt.Errorf("failed to read comp %v", i)
		}
		if typ > compConstMask|compSizeMask {
			return nil, fmt.Errorf("bad comp %v type %v", i, typ)
		}
		var op1, op2 uint64
		var ok1, ok2 bool
		if typ&compSizeMask == compSize8 {
			op1, ok1 = readUint64(outp)
			op2, ok2 = readUint64(outp)
		} else {
			var tmp1, tmp2 uint32
			tmp1, ok1 = readUint32(outp)
			tmp2, ok2 = readUint32(outp)
			op1, op2 = uint64(tmp1), uint64(tmp2)
		}
		if !ok1 || !ok2 {
			return nil, fmt.Errorf("failed to read comp %v op", i)
		}
		if op1 == op2 {
			continue // it's useless to store such comparisons
		}
		compMap.AddComp(op2, op1)
		if (typ & compConstMask) != 0 {
			// If one of the operands was const, then this operand is always
			// placed first in the instrumented callbacks. Such an operand
			// could not be an argument of our syscalls (because otherwise
			// it wouldn't be const), thus we simply ignore it.
			continue
		}
		compMap.AddComp(op1, op2)
	}
	return compMap, nil
}

func readUint32(outp *[]byte) (uint32, bool) {
	out := *outp
	if len(out) < 4 {
		return 0, false
	}
	v := *(*uint32)(unsafe.Pointer(&out[0]))
	*outp = out[4:]
	return v, true
}

func readUint64(outp *[]byte) (uint64, bool) {
	out := *outp
	if len(out) < 8 {
		return 0, false
	}
	v := *(*uint64)(unsafe.Pointer(&out[0]))
	*outp = out[8:]
	return v, true
}

func readUint32Array(outp *[]byte, size uint32) ([]uint32, bool) {
	out := *outp
	if int(size)*4 > len(out) {
		return nil, false
	}
	arr := ((*[1 << 28]uint32)(unsafe.Pointer(&out[0])))
	res := arr[:size:size]
	*outp = out[size*4:]
	return res, true
}

type command struct {
	pid      int
	config   *Config
	timeout  time.Duration
	cmd      *exec.Cmd
	dir      string
	readDone chan []byte
	exited   chan struct{}
	inrp     *os.File
	outwp    *os.File
	outmem   []byte
}

const (
	inMagic  = uint64(0xbadc0ffeebadface)
	outMagic = uint32(0xbadf00d)

	crashMagic = uint32(0xbadcbadc)
)

type handshakeReq struct {
	magic uint64
	flags uint64 // env flags
	pid   uint64
}

type handshakeReply struct {
	magic uint32
}

type executeReq struct {
	magic     uint64
	envFlags  uint64 // env flags
	execFlags uint64 // exec flags
	pid       uint64
	faultCall uint64
	faultNth  uint64
	progSize  uint64
	// prog follows on pipe or in shmem
}

type executeReply struct {
	magic uint32
	// If done is 0, then this is call completion message followed by callReply.
	// If done is 1, then program execution is finished and status is set.
	done   uint32
	status uint32
	stats  [StatCount + TStatCount + DPStatCount + CPStatCount*CPItemCount]uint32
}

type callReply struct {
	index      uint32 // call index in the program
	num        uint32 // syscall number (for cross-checking)
	errno      uint32
	flags      uint32 // see CallFlags
	signalSize uint32
	coverSize  uint32
	compsSize  uint32
	// signal/cover/comps follow
}

func makeCommand(pid int, bin []string, config *Config, inFile, outFile *os.File, inmem, outmem []byte,
	tmpDirPath string) (*command, error) {
	dir, err := ioutil.TempDir(tmpDirPath, "syzkaller-testdir")
	if err != nil {
		return nil, fmt.Errorf("failed to create temp dir: %v", err)
	}
	dir = osutil.Abs(dir)

	log.Logf(0, "makeCommand with bin:%v", bin)

	c := &command{
		pid:     pid,
		config:  config,
		timeout: sanitizeTimeout(config),
		dir:     dir,
		outmem:  outmem,
	}
	defer func() {
		if c != nil {
			c.close()
		}
	}()

	if err := os.Chmod(dir, 0777); err != nil {
		return nil, fmt.Errorf("failed to chmod temp dir: %v", err)
	}

	// Output capture pipe.
	rp, wp, err := os.Pipe()
	if err != nil {
		return nil, fmt.Errorf("failed to create pipe: %v", err)
	}
	defer wp.Close()

	// executor->ipc command pipe.
	inrp, inwp, err := os.Pipe()
	if err != nil {
		return nil, fmt.Errorf("failed to create pipe: %v", err)
	}
	defer inwp.Close()
	c.inrp = inrp

	// ipc->executor command pipe.
	outrp, outwp, err := os.Pipe()
	if err != nil {
		return nil, fmt.Errorf("failed to create pipe: %v", err)
	}
	defer outrp.Close()
	c.outwp = outwp

	c.readDone = make(chan []byte, 1)
	c.exited = make(chan struct{})

	cmd := osutil.Command(bin[0], bin[1:]...)
	if inFile != nil && outFile != nil {
		cmd.ExtraFiles = []*os.File{inFile, outFile}
	}
	cmd.Env = []string{}
	cmd.Dir = dir
	cmd.Stdin = outrp
	cmd.Stdout = inwp
	if config.Flags&FlagDebug != 0 {
		close(c.readDone)
		cmd.Stderr = os.Stdout
	} else {
		cmd.Stderr = wp
		go func(c *command) {
			// Read out output in case executor constantly prints something.
			const bufSize = 128 << 10
			output := make([]byte, bufSize)
			var size uint64
			for {
				n, err := rp.Read(output[size:])
				if n > 0 {
					size += uint64(n)
					if size >= bufSize*3/4 {
						copy(output, output[size-bufSize/2:size])
						size = bufSize / 2
					}
				}
				if err != nil {
					rp.Close()
					c.readDone <- output[:size]
					close(c.readDone)
					return
				}
			}
		}(c)
	}
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("failed to start executor binary: %v", err)
	}
	c.cmd = cmd
	wp.Close()
	// Note: we explicitly close inwp before calling handshake even though we defer it above.
	// If we don't do it and executor exits before writing handshake reply,
	// reading from inrp will hang since we hold another end of the pipe open.
	inwp.Close()

	if c.config.UseForkServer {
		if err := c.handshake(); err != nil {
			return nil, err
		}
	}
	tmp := c
	c = nil // disable defer above
	return tmp, nil
}

// PeriScope
func makeCommandWithPipes(pid int, bin []string, config *Config, inFile, outFile, stPipe, ctlPipe *os.File, inmem, outmem []byte,
	tmpDirPath string) (*command, error) {
	dir, err := ioutil.TempDir(tmpDirPath, "syzkaller-testdir")
	if err != nil {
		return nil, fmt.Errorf("failed to create temp dir: %v", err)
	}
	dir = osutil.Abs(dir)

	log.Logf(0, "makeCommandWithPipes bin:%v", bin)

	c := &command{
		pid:     pid,
		config:  config,
		timeout: sanitizeTimeout(config),
		dir:     dir,
		outmem:  outmem,
	}
	defer func() {
		if c != nil {
			c.close()
		}
	}()

	if err := os.Chmod(dir, 0777); err != nil {
		return nil, fmt.Errorf("failed to chmod temp dir: %v", err)
	}

	// executor->ipc command pipe.
	c.inrp = config.OutPipe

	// ipc->executor command pipe.
	c.outwp = config.InPipe

	c.readDone = make(chan []byte, 1)
	c.exited = make(chan struct{})

	cmd := osutil.Command(bin[0], bin[1:]...)
	if inFile != nil && outFile != nil {
		cmd.ExtraFiles = []*os.File{inFile, outFile}
	}
	cmd.Env = []string{}
	cmd.Dir = dir
	if config.Flags&FlagDebug != 0 {
		close(c.readDone)
		go io.Copy(os.Stdout, config.ErrPipe)
	} else if !config.UseForkServer {
		close(c.readDone)
		// TODO: read out output after execution failure.
	} else {
		go func(c *command) {
			// Read out output in case executor constantly prints something.
			const bufSize = 128 << 10
			output := make([]byte, bufSize)
			var size uint64
			for {
				n, err := config.ErrPipe.Read(output[size:])
				if n > 0 {
					// log.Logf(0, "read %v bytes from ErrPipe", n)
					size += uint64(n)
					if size >= bufSize*3/4 {
						copy(output, output[size-bufSize/2:size])
						size = bufSize / 2
					}
					// log.Logf(0, "%s", output)
				}
				if err != nil {
					log.Logf(0, "reading from ErrPipe failed %v", err)
					//config.ErrPipe.Close()
					c.readDone <- output[:size]
					close(c.readDone)
					return
				}
			}
		}(c)
	}

	c.cmd = cmd

	log.Logf(0, "performing handshake with an already running executor...")
	if c.config.UseForkServer {
		if err := c.handshake(); err != nil {
			return nil, err
		}
	}
	log.Logf(0, "handshake successful.")
	tmp := c
	c = nil // disable defer above
	return tmp, nil
}

func (c *command) close() {
	if c.cmd != nil && c.cmd.Process == nil {
		return
	}
	if c.cmd != nil {
		c.cmd.Process.Kill()
		c.wait()
	}
	osutil.RemoveAll(c.dir)
	if c.inrp != nil {
		c.inrp.Close()
	}
	if c.outwp != nil {
		c.outwp.Close()
	}
}

// handshake sends handshakeReq and waits for handshakeReply.
func (c *command) handshake() error {
	req := &handshakeReq{
		magic: inMagic,
		flags: uint64(c.config.Flags),
		pid:   uint64(c.pid),
	}
	reqData := (*[unsafe.Sizeof(*req)]byte)(unsafe.Pointer(req))[:]
	if _, err := c.outwp.Write(reqData); err != nil {
		return c.handshakeError(fmt.Errorf("failed to write control pipe: %v", err))
	}

	read := make(chan error, 1)
	go func() {
		reply := &handshakeReply{}
		replyData := (*[unsafe.Sizeof(*reply)]byte)(unsafe.Pointer(reply))[:]
		if _, err := io.ReadFull(c.inrp, replyData); err != nil {
			read <- err
			return
		}
		if reply.magic != outMagic {
			read <- fmt.Errorf("bad handshake reply magic 0x%x", reply.magic)
			return
		}
		read <- nil
	}()
	// Sandbox setup can take significant time.
	timeout := time.NewTimer(time.Minute)
	select {
	case err := <-read:
		timeout.Stop()
		if err != nil {
			return c.handshakeError(err)
		}
		return nil
	case <-timeout.C:
		return c.handshakeError(fmt.Errorf("not serving"))
	}
}

func (c *command) handshakeError(err error) error {
	if c.cmd.Process == nil {
		output := <-c.readDone
		err = fmt.Errorf("executor %v: %v\n%s", c.pid, err, output)
		return err
	}
	c.cmd.Process.Kill()
	output := <-c.readDone
	err = fmt.Errorf("executor %v: %v\n%s", c.pid, err, output)
	c.wait()
	return err
}

func (c *command) wait() error {
	err := c.cmd.Wait()
	select {
	case <-c.exited:
		// c.exited closed by an earlier call to wait.
	default:
		close(c.exited)
	}
	return err
}

func (c *command) exec(opts *ExecOpts, progData []byte) (output []byte, stats [StatCount + TStatCount + DPStatCount + CPStatCount*CPItemCount]uint32, hanged bool, crashed bool, err0 error) {
	req := &executeReq{
		magic:     inMagic,
		envFlags:  uint64(c.config.Flags),
		execFlags: uint64(opts.Flags),
		pid:       uint64(c.pid),
		faultCall: uint64(opts.FaultCall),
		faultNth:  uint64(opts.FaultNth),
		progSize:  uint64(len(progData)),
	}

	reqData := (*[unsafe.Sizeof(*req)]byte)(unsafe.Pointer(req))[:]
	if _, err := c.outwp.Write(reqData); err != nil {
		output = <-c.readDone
		err0 = fmt.Errorf("executor %v: failed to write control pipe: %v", c.pid, err)
		return
	}
	if progData != nil {
		if _, err := c.outwp.Write(progData); err != nil {
			output = <-c.readDone
			err0 = fmt.Errorf("executor %v: failed to write control pipe: %v", c.pid, err)
			return
		}
	}
	// At this point program is executing.

	done := make(chan bool)
	hang := make(chan bool)
	go func() {
		t := time.NewTimer(c.timeout)
		select {
		case <-t.C:
			if c.cmd.Process != nil {
				c.cmd.Process.Kill()
			}
			hang <- true
		case <-done:
			t.Stop()
			hang <- false
		}
	}()
	exitStatus := -1
	completedCalls := (*uint32)(unsafe.Pointer(&c.outmem[0]))
	outmem := c.outmem[4:]
	for {
		reply := &executeReply{}
		replyData := (*[unsafe.Sizeof(*reply)]byte)(unsafe.Pointer(reply))[:]
		if _, err := io.ReadFull(c.inrp, replyData); err != nil {
			break
		}
		if reply.magic != outMagic && reply.magic != crashMagic {
			fmt.Fprintf(os.Stderr, "executor %v: got bad reply magic 0x%x\n", c.pid, reply.magic)
			os.Exit(1)
		}
		for stat := Stat(0); stat < StatCount+TStatCount+DPStatCount+CPStatCount*CPItemCount; stat++ {
			stats[stat] = reply.stats[stat]
		}
		if reply.magic == crashMagic {
			crashed = true
		}
		if reply.done != 0 {
			exitStatus = int(reply.status)
			break
		}
		callReply := &callReply{}
		callReplyData := (*[unsafe.Sizeof(*callReply)]byte)(unsafe.Pointer(callReply))[:]
		if _, err := io.ReadFull(c.inrp, callReplyData); err != nil {
			break
		}
		if callReply.signalSize != 0 || callReply.coverSize != 0 || callReply.compsSize != 0 {
			// This is unsupported yet.
			fmt.Fprintf(os.Stderr, "executor %v: got call reply with coverage %d %d %d\n", c.pid, callReply.signalSize, callReply.coverSize, callReply.compsSize)
			os.Exit(1)
			break
		}
		copy(outmem, callReplyData)
		outmem = outmem[len(callReplyData):]
		*completedCalls++
	}
	close(done)
	if exitStatus == 0 {
		// Program was OK.
		<-hang
		return
	}

	if c.cmd.Process != nil {
		c.cmd.Process.Kill()
	}
	output = <-c.readDone
	if <-hang {
		hanged = true
		var err error
		if err != nil {
			output = append(output, err.Error()...)
			output = append(output, '\n')
			err0 = fmt.Errorf("executor %v: hang\n%s", c.pid, output)
			log.Logf(0, "%v", err0)
		}
		return
	}
	// if exitStatus == -1 {
	// exitStatus = osutil.ProcessExitStatus(c.cmd.ProcessState)
	//}
	// Ignore all other errors.
	// Without fork server executor can legitimately exit (program contains exit_group),
	// with fork server the top process can exit with statusFail if it wants special handling.
	//if exitStatus == statusFail {
	// PeriScope: USB fuzzing process must not exit
	if exitStatus == -1 {
		err0 = fmt.Errorf("executor %v: exit status %d\n%s", c.pid, exitStatus, output)
	}
	log.Logf(0, "%v", err0)
	return
}

func sanitizeTimeout(config *Config) time.Duration {
	const (
		executorTimeout = 5 * time.Second
		minTimeout      = executorTimeout + 2*time.Second
	)
	timeout := config.Timeout
	if timeout == 0 {
		// Executor protects against most hangs, so we use quite large timeout here.
		// Executor can be slow due to global locks in namespaces and other things,
		// so let's better wait than report false misleading crashes.
		timeout = time.Minute
		if !config.UseForkServer {
			// If there is no fork server, executor does not have internal timeout.
			timeout = executorTimeout
		}
	}
	// IPC timeout must be larger then executor timeout.
	// Otherwise IPC will kill parent executor but leave child executor alive.
	if config.UseForkServer && timeout < minTimeout {
		timeout = minTimeout
	}
	return timeout
}
