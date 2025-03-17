// Copyright 2015 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package main

import (
	"bytes"
	"encoding/hex"
	"flag"
	"fmt"
	"io"
	"math/rand"
	"net/http"
	_ "net/http/pprof"
	"os"
	"runtime"
	"runtime/debug"
	"sort"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"

	"github.com/google/syzkaller/moneta"
	"github.com/google/syzkaller/pkg/csource"
	"github.com/google/syzkaller/pkg/hash"
	"github.com/google/syzkaller/pkg/host"
	"github.com/google/syzkaller/pkg/ipc"
	"github.com/google/syzkaller/pkg/ipc/ipcconfig"
	"github.com/google/syzkaller/pkg/log"
	"github.com/google/syzkaller/pkg/osutil"
	"github.com/google/syzkaller/pkg/rpctype"
	"github.com/google/syzkaller/pkg/signal"
	"github.com/google/syzkaller/prog"
	_ "github.com/google/syzkaller/sys"
)

type Fuzzer struct {
	name              string
	outputType        OutputType
	config            *ipc.Config
	execOpts          *ipc.ExecOpts
	procs             []*Proc
	gate              *ipc.Gate
	workQueue         *WorkQueue
	needPoll          chan struct{}
	choiceTable       *prog.ChoiceTable
	stats             [StatCount]uint64
	statsAgamotto     [ipc.StatCount + ipc.TStatCount*ipc.TBucketCount + ipc.DPStatCount*ipc.DPBucketCount + ipc.CPStatCount*ipc.CPBucketCount]uint64
	manager           *rpctype.RPCClient
	target            *prog.Target
	triagedCandidates uint32

	faultInjectionEnabled    bool
	comparisonTracingEnabled bool

	corpusMu     sync.RWMutex
	corpus       []*prog.Prog
	corpusHashes map[hash.Sig]struct{}
	corpusPrios  []int64
	sumPrios     int64

	signalMu     sync.RWMutex
	corpusSignal signal.Signal // signal of inputs in corpus
	maxSignal    signal.Signal // max signal ever observed including flakes
	newSignal    signal.Signal // diff of maxSignal since last sync with master

	progSignal []signal.Signal // PeriScope

	logMu sync.Mutex

	// Snappuzz
	fileDescriptorDB []int
	snapPoint        int
	fdCount          []int
}

type FuzzerSnapshot struct {
	corpus      []*prog.Prog
	corpusPrios []int64
	sumPrios    int64
	// snapPoint int
}

type Stat int

const (
	StatGenerate Stat = iota
	StatFuzz
	StatCandidate
	StatTriage
	StatMinimize
	StatMinimizeS
	StatMinimizeN
	StatMinimizeF
	StatMinimizeC
	StatMinimizeR
	StatSmash
	StatHint
	StatSeed
	StatSquash
	StatSplice
	StatInsCall
	StatMutArg
	StatRmCall
	StatCount
)

var statNames = [StatCount]string{
	StatGenerate:  "exec gen",
	StatFuzz:      "exec fuzz",
	StatCandidate: "exec candidate",
	StatTriage:    "exec triage",
	StatMinimize:  "exec minimize",
	StatMinimizeS: "exec minimize (success)",
	StatMinimizeN: "exec minimize (failure)",
	StatMinimizeF: "exec minimize (exec failure)",
	StatMinimizeC: "exec minimize (call failure)",
	StatMinimizeR: "exec minimize (cov failure)",
	StatSmash:     "exec smash",
	StatHint:      "exec hints",
	StatSeed:      "exec seeds",
	StatSquash:    "mutate squash",
	StatSplice:    "mutate splice",
	StatInsCall:   "mutate insert call",
	StatMutArg:    "mutate mutate arg",
	StatRmCall:    "mutate remove call",
}

type OutputType int

const (
	OutputNone OutputType = iota
	OutputStdout
	OutputDmesg
	OutputFile
)

func main() {
	debug.SetGCPercent(50)

	var VMArch string
	var ExecMode bool

	VMArch = runtime.GOARCH
	ExecMode = true

	if moneta.MonetaVMArch == "arm64" {
		VMArch = moneta.MonetaVMArch
		ExecMode = false
	}

	log.Logf(0, "VMArch %v", VMArch)

	var (
		flagName    = flag.String("name", "test", "unique name for manager")
		flagOS      = flag.String("os", runtime.GOOS, "target OS")
		flagArch    = flag.String("arch", VMArch, "target arch")
		flagManager = flag.String("manager", "", "manager rpc address")
		flagProcs   = flag.Int("procs", 1, "number of parallel test processes")
		flagOutput  = flag.String("output", "stdout", "write programs to none/stdout/dmesg/file")
		flagPprof   = flag.String("pprof", "", "address to serve pprof profiles")
		flagTest    = flag.Bool("test", false, "enable image testing mode")      // used by syz-ci
		flagRunTest = flag.Bool("runtest", false, "enable program testing mode") // used by pkg/runtest
		flagArgs    = flag.String("args", "", "string to be parsed as args")
		flagStPipe  = flag.Uint64("st_pipe", 0, "st_pipe to VMM")                          // fuzzer <-> VMM
		flagCtlPipe = flag.Uint64("ctl_pipe", 0, "ctl_pipe to VMM")                        // fuzzer <-> VMM
		flagShmId   = flag.String("shm_id", "", "name of shared memory to be shm_open'ed") // fuzzer <-> VMM
		flagInPipe  = flag.Uint64("in_pipe", 0, "in_pipe to executor running inside VM")   // fuzzer <-> executor
		flagOutPipe = flag.Uint64("out_pipe", 0, "out_pipe to executor running inside VM") // fuzzer <-> executor
		flagErrPipe = flag.Uint64("err_pipe", 0, "err_pipe to executor running inside VM") // fuzzer <-> executor
		flagIndex   = flag.Int("index", 1, "VM/fuzzer index")
		//flagStdout  = flag.Uint64("stdout", 0, "Replace Stdout")                           // fuzzer -> manager
		//flagStderr  = flag.Uint64("stderr", 0, "Replace Stderr")                           // fuzzer -> manager
	)
	flag.Parse()

	outputType := parseOutputType(*flagOutput)
	log.Logf(0, "fuzzer started")
	log.Logf(0, "os=%v, arch=%v", *flagOS, *flagArch)

	target, err := prog.GetTarget(*flagOS, *flagArch)
	if err != nil {
		log.Fatalf("%v", err)
	}

	config, execOpts, err := ipcconfig.Default(target)
	if err != nil {
		log.Fatalf("failed to create default ipc config: %v", err)
	}

	if *flagArgs != "" {
		flags := flag.NewFlagSet("", flag.ContinueOnError)
		flagName = flags.String("name", "test", "unique name for manager")
		flagOS = flags.String("os", runtime.GOOS, "target OS")
		flagArch = flags.String("arch", runtime.GOARCH, "target arch")
		flagManager = flags.String("manager", "", "manager rpc address")
		flagProcs = flags.Int("procs", 1, "number of parallel test processes")
		flagOutput = flags.String("output", "stdout", "write programs to none/stdout/dmesg/file")
		flagPprof = flags.String("pprof", "", "address to serve pprof profiles")
		flagTest = flags.Bool("test", false, "enable image testing mode")         // used by syz-ci
		flagRunTest = flags.Bool("runtest", false, "enable program testing mode") // used by pkg/runtest
		_flagThreaded := flags.Bool("threaded", false, "use threaded mode in executor")
		_flagCollide := flags.Bool("collide", false, "collide syscalls to provoke data races")
		_flagSignal := flags.Bool("cover", false, "collect feedback signals (coverage)")
		_flagVirtCover := flags.Bool("vcover", false, "collect feedback signal (coverage) from VMM")
		_flagRootChkptOnly := flags.Bool("rootchkptonly", false, "checkpoint only once")
		_flagSandbox := flags.String("sandbox", "none", "sandbox for fuzzing (none/setuid/namespace/android_untrusted_app)")
		_flagDebug := flags.Bool("debug", false, "debug output from executor")
		_flagSeed := flags.Int64("seed", -1, "remove randomness for benchmark")
		_flagExecveTracee := flags.Bool("ExecveTracee", ExecMode, "..")
		args := strings.Split(*flagArgs, " ")
		log.Logf(0, "parsing args %v", args)

		if err := flags.Parse(args); err != nil {
			log.Fatalf("parsing args failed err=%v", err)
		}

		// execOpts won't have any effect
		if *_flagThreaded {
			execOpts.Flags |= ipc.FlagThreaded
		}

		if *_flagCollide {
			execOpts.Flags |= ipc.FlagCollide
		}

		if *_flagSignal {
			config.Flags |= ipc.FlagSignal
		}

		if *_flagVirtCover {
			config.Flags |= ipc.FlagVirtCover
		}

		if *_flagRootChkptOnly {
			config.Flags |= ipc.FlagRootChkptOnly
		}
		if *_flagExecveTracee {
			config.Flags |= ipc.FlagExecveTracee
		}

		sandboxFlags, err := ipc.SandboxToFlags(*_flagSandbox)
		if err != nil {
			log.Fatalf("parsing sandbox flag failed err=%v", err)
		}
		config.Flags |= sandboxFlags

		if *_flagDebug {
			config.Flags |= ipc.FlagDebug
		}

		config.Seed = *_flagSeed
	}

	sandbox := ipc.FlagsToSandbox(config.Flags)
	shutdown := make(chan struct{})
	osutil.HandleInterrupts(shutdown)
	go func() {
		// Handles graceful preemption on GCE.
		<-shutdown
		log.Logf(0, "SYZ-FUZZER: PREEMPTED")
		os.Exit(1)
	}()

	checkArgs := &checkArgs{
		target:      target,
		sandbox:     sandbox,
		ipcConfig:   config,
		ipcExecOpts: execOpts,
	}
	if *flagTest {
		testImage(*flagManager, checkArgs)
		return
	}

	if *flagPprof != "" {
		go func() {
			err := http.ListenAndServe(*flagPprof, nil)
			log.Fatalf("failed to serve pprof profiles: %v", err)
		}()
	} else {
		runtime.MemProfileRate = 0
	}

	if *flagShmId != "" {
		fd, err := syscall.Open("/dev/shm/"+*flagShmId, syscall.O_RDWR|syscall.O_CLOEXEC, 0644)
		if err != nil {
			log.Fatalf("shm open failed: id=%v error=%v", *flagShmId, err)
		}
		mem, err := syscall.Mmap(fd, 0, int((256<<10)*unsafe.Sizeof(uintptr(0))*2),
			syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
		if err != nil {
			log.Fatalf("shm mmap failed: fd=%v error=%v", fd, err)
		}
		copy(mem[0:], []byte{0xef, 0xbe, 0xef, 0xbe})
		log.Logf(0, "shm initialized: id=%v fd=%v", *flagShmId, fd)
	}

	var ctl_wpipe *os.File
	if *flagCtlPipe > 0 {
		ctl_wpipe = os.NewFile(uintptr(*flagCtlPipe), "ctl_pipe")
		config.CtlPipe = ctl_wpipe
		b := []byte{0x0d, 0xf0, 0xad, 0xde}
		ctl_wpipe.Write(b)
	}

	var st_rpipe *os.File
	if *flagStPipe > 0 {
		st_rpipe = os.NewFile(uintptr(*flagStPipe), "st_pipe")
		config.StPipe = st_rpipe
		var buf [4]byte
		n, err := io.ReadFull(st_rpipe, buf[:])
		if err != nil {
			log.Fatalf("out pipe read failed: %v", err)
		}
		if n != 4 {
			log.Fatalf("out pipe returned more than 4 bytes")
		}
		if !bytes.Equal(buf[:], []byte{0x0d, 0xf0, 0xef, 0xbe}) {
			log.Fatalf("unexpected out pipe return value %v", hex.Dump(buf[:]))
		}
	}

	log.Logf(0, "dialing manager at %v", *flagManager)
	manager, err := rpctype.NewRPCClient(*flagManager)
	if err != nil {
		log.Fatalf("failed to connect to manager: %v ", err)
	}
	a := &rpctype.ConnectArgs{Name: *flagName}
	r := &rpctype.ConnectRes{}
	if err := manager.Call("Manager.Connect", a, r); err != nil {
		log.Fatalf("failed to connect to manager: %v ", err)
	}
	featureFlags, err := csource.ParseFeaturesFlags("none", "none", true)
	if err != nil {
		log.Fatal(err)
	}

	log.Logf(0, "setting up pipes to executor...")

	var in_wpipe *os.File
	if *flagInPipe > 0 {
		in_wpipe = os.NewFile(uintptr(*flagInPipe), "in_pipe")
	} else {
		in_wpipe = ctl_wpipe
	}
	if in_wpipe != nil {
		config.InPipe = in_wpipe
		//b := []byte{0xad, 0xde, 0xad, 0xde}
		//in_wpipe.Write(b)
	}

	var out_rpipe *os.File
	if *flagOutPipe > 0 {
		out_rpipe = os.NewFile(uintptr(*flagOutPipe), "out_pipe")
	} else {
		out_rpipe = st_rpipe
	}
	if out_rpipe != nil {
		config.OutPipe = out_rpipe
	}

	var err_rpipe *os.File
	if *flagErrPipe > 0 {
		err_rpipe = os.NewFile(uintptr(*flagErrPipe), "err_pipe")
		config.ErrPipe = err_rpipe
	}

	log.Logf(0, "all three (in/out/err) pipes to executor successfully set up")

	if r.CheckResult == nil {
		checkArgs.gitRevision = r.GitRevision
		checkArgs.targetRevision = r.TargetRevision
		checkArgs.enabledCalls = r.EnabledCalls
		checkArgs.allSandboxes = r.AllSandboxes
		checkArgs.featureFlags = featureFlags
		r.CheckResult, err = checkMachine(checkArgs)
		if err != nil {
			if r.CheckResult == nil {
				r.CheckResult = new(rpctype.CheckArgs)
			}
			r.CheckResult.Error = err.Error()
		}
		r.CheckResult.Name = *flagName
		if err := manager.Call("Manager.Check", r.CheckResult, nil); err != nil {
			log.Fatalf("Manager.Check call failed: %v", err)
		}
		if r.CheckResult.Error != "" {
			log.Fatalf("%v", r.CheckResult.Error)
		}
	} else {
		if err = host.Setup(target, r.CheckResult.Features, featureFlags, config.Executor); err != nil {
			log.Fatal(err)
		}
	}
	log.Logf(0, "syscalls: %v", len(r.CheckResult.EnabledCalls[sandbox]))
	for _, feat := range r.CheckResult.Features.Supported() {
		log.Logf(0, "%v: %v", feat.Name, feat.Reason)
	}
	if r.CheckResult.Features[host.FeatureExtraCoverage].Enabled {
		config.Flags |= ipc.FlagExtraCover
	}
	if r.CheckResult.Features[host.FeatureNetInjection].Enabled {
		config.Flags |= ipc.FlagEnableTun
	}
	if r.CheckResult.Features[host.FeatureNetDevices].Enabled {
		config.Flags |= ipc.FlagEnableNetDev
	}

	if false {
		config.Flags |= ipc.FlagEnableNetReset
		config.Flags |= ipc.FlagEnableCgroups
	}

	//config.Flags |= ipc.FlagEnableCloseFds
	if r.CheckResult.Features[host.FeatureDevlinkPCI].Enabled {
		config.Flags |= ipc.FlagEnableDevlinkPCI
	}

	if *flagRunTest {
		runTest(target, manager, *flagName, config.Executor)
		return
	}

	// Maybe not in use now ...
	// Initial design of Moneta
	// fa := &rpctype.FiledescriptorArgs{
	// 	Name: "0",
	// }
	// fr := &rpctype.FiledescriptorRes{}
	// if err := manager.Call("Manager.GetFileDescriptor", fa, fr); err != nil {
	// 	log.Fatalf("Manager.GetFileDescriptor call failed: %v", err)
	// }

	fuzzerIndex, err := strconv.Atoi(strings.Split(*flagName, "-")[1])
	if err != nil {
		log.Fatalf("Index is not conveted!")
	}

	fa := &rpctype.FdCountArgs{
		Index: fuzzerIndex,
	}
	fr := &rpctype.FdCountRes{}
	if err := manager.Call("Manager.GetfdCount", fa, fr); err != nil {
		log.Fatalf("Manager.GetfdCount call failed: %v", err)
	}

	needPoll := make(chan struct{}, 1)
	needPoll <- struct{}{}
	fuzzer := &Fuzzer{
		name:                     *flagName,
		outputType:               outputType,
		config:                   config,
		execOpts:                 execOpts,
		workQueue:                newWorkQueue(*flagProcs, needPoll),
		needPoll:                 needPoll,
		manager:                  manager,
		target:                   target,
		faultInjectionEnabled:    r.CheckResult.Features[host.FeatureFault].Enabled,
		comparisonTracingEnabled: r.CheckResult.Features[host.FeatureComparisons].Enabled,
		corpusHashes:             make(map[hash.Sig]struct{}),
		// fileDescriptorDB:         fr.FileDescriptorDB,
		fdCount:   fr.FdCount,
		snapPoint: fr.SnapPoint,
	}
	// fuzzer.snapPoint = strings.Split(fuzzer.name, "-")[1] + 1
	// fmt.Printf("fuzzer name: %v\n", fuzzer.name)
	gateCallback := fuzzer.useBugFrames(r, *flagProcs)
	fuzzer.gate = ipc.NewGate(2**flagProcs, gateCallback)

	for i := 0; fuzzer.poll(i == 0, nil); i++ {
	}
	calls := make(map[*prog.Syscall]bool)
	for _, id := range r.CheckResult.EnabledCalls[sandbox] {
		calls[target.Syscalls[id]] = true
	}
	prios := target.CalculatePriorities(fuzzer.corpus)
	fuzzer.choiceTable = target.BuildChoiceTable(prios, calls)

	fuzzer.config.Index = *flagIndex
	*flagProcs = 1

	for pid := 0; pid < *flagProcs; pid++ {
		env, err := ipc.MakeEnv(fuzzer.config, pid)
		if err != nil {
			log.Fatalf("failed to make env: %v", err)
		}

		// PeriScope: should probably be reenabled later
		//checkSimpleProgramInExistingEnv(env, checkArgs)

		proc, err := newProc(fuzzer, pid, env)
		if err != nil {
			log.Fatalf("failed to create proc: %v", err)
		}
		fuzzer.procs = append(fuzzer.procs, proc)
		go proc.loop()
	}

	fuzzer.pollLoop()
}

// Returns gateCallback for leak checking if enabled.
func (fuzzer *Fuzzer) useBugFrames(r *rpctype.ConnectRes, flagProcs int) func() {
	var gateCallback func()

	if r.CheckResult.Features[host.FeatureLeak].Enabled {
		gateCallback = func() { fuzzer.gateCallback(r.MemoryLeakFrames) }
	}

	if r.CheckResult.Features[host.FeatureKCSAN].Enabled && len(r.DataRaceFrames) != 0 {
		fuzzer.blacklistDataRaceFrames(r.DataRaceFrames)
	}

	return gateCallback
}

func (fuzzer *Fuzzer) gateCallback(leakFrames []string) {
	// Leak checking is very slow so we don't do it while triaging the corpus
	// (otherwise it takes infinity). When we have presumably triaged the corpus
	// (triagedCandidates == 1), we run leak checking bug ignore the result
	// to flush any previous leaks. After that (triagedCandidates == 2)
	// we do actual leak checking and report leaks.
	triagedCandidates := atomic.LoadUint32(&fuzzer.triagedCandidates)
	if triagedCandidates == 0 {
		return
	}
	args := append([]string{"leak"}, leakFrames...)
	output, err := osutil.RunCmd(10*time.Minute, "", fuzzer.config.Executor, args...)
	if err != nil && triagedCandidates == 2 {
		// If we exit right away, dying executors will dump lots of garbage to console.
		os.Stdout.Write(output)
		fmt.Printf("BUG: leak checking failed")
		time.Sleep(time.Hour)
		os.Exit(1)
	}
	if triagedCandidates == 1 {
		atomic.StoreUint32(&fuzzer.triagedCandidates, 2)
	}
}

func (fuzzer *Fuzzer) blacklistDataRaceFrames(frames []string) {
	args := append([]string{"setup_kcsan_blacklist"}, frames...)
	output, err := osutil.RunCmd(10*time.Minute, "", fuzzer.config.Executor, args...)
	if err != nil {
		log.Fatalf("failed to set KCSAN blacklist: %v", err)
	}
	log.Logf(0, "%s", output)
}

func (fuzzer *Fuzzer) pollLoop() {
	var execTotal uint64
	var lastPoll time.Time
	var lastPrint time.Time
	ticker := time.NewTicker(3 * time.Second).C
	for {
		poll := false
		select {
		case <-ticker:
		case <-fuzzer.needPoll:
			poll = true
		}
		if fuzzer.outputType != OutputStdout && time.Since(lastPrint) > 10*time.Second {
			// Keep-alive for manager.
			log.Logf(0, "alive, executed %v", execTotal)
			lastPrint = time.Now()
		}
		if poll || time.Since(lastPoll) > 10*time.Second {
			log.Logf(0, "poll: poll=%v, last=%v, time=%v\n", poll, lastPoll, time.Since(lastPoll) > 10*time.Second)
			needCandidates := fuzzer.workQueue.wantCandidates()
			if poll && !needCandidates {
				continue
			}
			stats := make(map[string]uint64)
			for _, proc := range fuzzer.procs {
				stats["exec total"] += atomic.SwapUint64(&proc.env.StatExecs, 0)
				stats["executor restarts"] += atomic.SwapUint64(&proc.env.StatRestarts, 0)
			}
			for stat := Stat(0); stat < StatCount; stat++ {
				v := atomic.SwapUint64(&fuzzer.stats[stat], 0)
				stats[statNames[stat]] = v
			}
			for stat := ipc.Stat(0); stat < ipc.StatCount; stat++ {
				v := atomic.SwapUint64(&fuzzer.statsAgamotto[stat], 0)
				if stat == ipc.StatTimeSaved || stat == ipc.StatTimeExec {
					atomic.AddUint64(&fuzzer.statsAgamotto[stat], v%1000)
					v = v / 1000
				}
				stats[ipc.StatNames[stat]] = v
			}
			for stat := ipc.Stat(0); stat < ipc.TStatCount; stat++ {
				for bucket := ipc.Stat(0); bucket < ipc.TBucketCount; bucket++ {
					v := atomic.SwapUint64(&fuzzer.statsAgamotto[ipc.StatCount+stat*ipc.TBucketCount+bucket], 0)
					statName := ipc.TStatNames[stat]
					var bucketName string
					if bucket == ipc.TBucketRest {
						bucketName = fmt.Sprintf(">%vs", float32(ipc.TBucketRanges[bucket-1])/float32(1000))
					} else {
						bucketName = fmt.Sprintf("<=%.2fs", float32(ipc.TBucketRanges[bucket])/float32(1000))
					}
					statName = fmt.Sprintf(statName, bucketName)
					stats[statName] = v
				}
			}
			for stat := ipc.Stat(0); stat < ipc.DPStatCount; stat++ {
				for bucket := ipc.Stat(0); bucket < ipc.DPBucketCount; bucket++ {
					v := atomic.SwapUint64(&fuzzer.statsAgamotto[ipc.StatCount+ipc.TStatCount*ipc.TBucketCount+stat*ipc.DPBucketCount+bucket], 0)
					statName := ipc.DPStatNames[stat]
					var bucketName string
					if bucket == ipc.DPBucketRest {
						bucketName = fmt.Sprintf(">%02dk", ipc.DPBucketRanges[bucket-1]/1000)
					} else if ipc.DPBucketRanges[bucket] < 1000 {
						bucketName = fmt.Sprintf("<=%.2fk", float32(ipc.DPBucketRanges[bucket])/float32(1000))
					} else {
						bucketName = fmt.Sprintf("<=%02dk", ipc.DPBucketRanges[bucket]/1000)
					}
					statName = fmt.Sprintf(statName, bucketName)
					stats[statName] = v
				}
			}
			for stat := ipc.Stat(0); stat < ipc.CPStatCount; stat++ {
				for bucket := ipc.Stat(0); bucket < ipc.CPBucketCount; bucket++ {
					v := atomic.SwapUint64(&fuzzer.statsAgamotto[ipc.StatCount+ipc.TStatCount*ipc.TBucketCount+ipc.DPStatCount*ipc.DPBucketCount+stat*ipc.CPBucketCount+bucket], 0)
					statName := ipc.CPStatNames[stat]
					var bucketName string
					if bucket == ipc.CPBucketRest {
						bucketName = fmt.Sprintf(">%02d", ipc.CPBucketRanges[bucket-1]/1024)
					} else {
						bucketName = fmt.Sprintf("<=%02d", ipc.CPBucketRanges[bucket]/1024)
					}
					statName = fmt.Sprintf(statName, bucketName)
					stats[statName] = v
				}
			}
			execTotal = stats["exec total"]
			if !fuzzer.poll(needCandidates, stats) {
				lastPoll = time.Now()
			}
		}
	}
}

func (fuzzer *Fuzzer) poll(needCandidates bool, stats map[string]uint64) bool {
	log.Logf(0, "poll: needCandidates=%v", needCandidates)
	a := &rpctype.PollArgs{
		Name:           fuzzer.name,
		NeedCandidates: needCandidates,
		MaxSignal:      fuzzer.grabNewSignal().Serialize(),
		Stats:          stats,
	}
	r := &rpctype.PollRes{}
	if err := fuzzer.manager.Call("Manager.Poll", a, r); err != nil {
		log.Fatalf("Manager.Poll call failed: %v", err)
	}
	maxSignal := r.MaxSignal.Deserialize()
	log.Logf(1, "poll: candidates=%v inputs=%v signal=%v",
		len(r.Candidates), len(r.NewInputs), maxSignal.Len())
	fuzzer.addMaxSignal(maxSignal)
	for _, inp := range r.NewInputs {
		fuzzer.addInputFromAnotherFuzzer(inp)
	}
	// fmt.Printf("len of candidates in fuzzer: %v %v\n", len(r.Candidates), a.Name)
	for _, candidate := range r.Candidates {
		p, err := fuzzer.target.Deserialize(candidate.Prog, prog.NonStrict)
		p.SnapPoint = candidate.SnapPoint
		if err != nil {
			log.Fatalf("failed to parse program from manager: %v", err)
		}
		flags := ProgCandidate
		if candidate.Minimized {
			flags |= ProgMinimized
		}
		if candidate.Smashed {
			flags |= ProgSmashed
		}
		fuzzer.workQueue.enqueue(&WorkCandidate{
			p:     p,
			flags: flags,
		})
	}
	if needCandidates && len(r.Candidates) == 0 && atomic.LoadUint32(&fuzzer.triagedCandidates) == 0 {
		atomic.StoreUint32(&fuzzer.triagedCandidates, 1)
	}
	return len(r.NewInputs) != 0 || len(r.Candidates) != 0 || maxSignal.Len() != 0
}

func (fuzzer *Fuzzer) sendInputToManager(inp rpctype.RPCInput) {
	a := &rpctype.NewInputArgs{
		Name:     fuzzer.name,
		RPCInput: inp,
	}
	if err := fuzzer.manager.Call("Manager.NewInput", a, nil); err != nil {
		log.Fatalf("Manager.NewInput call failed: %v", err)
	}
}

func (fuzzer *Fuzzer) addInputFromAnotherFuzzer(inp rpctype.RPCInput) {
	p, err := fuzzer.target.Deserialize(inp.Prog, prog.NonStrict)
	p.SnapPoint = inp.SnapPoint
	if err != nil {
		log.Fatalf("failed to deserialize prog from another fuzzer: %v", err)
	}
	sig := hash.Hash(inp.Prog)
	sign := inp.Signal.Deserialize()
	fuzzer.addInputToCorpus(p, sign, sig)
}

func (fuzzer *FuzzerSnapshot) chooseProgram(r *rand.Rand) (*prog.Prog, int) {
	randVal := r.Int63n(fuzzer.sumPrios + 1)
	idx := sort.Search(len(fuzzer.corpusPrios), func(i int) bool {
		return fuzzer.corpusPrios[i] >= randVal
	})
	energy := 1
	return fuzzer.corpus[idx], energy
}

func (fuzzer *FuzzerSnapshot) snapChooseProgram(r *rand.Rand, snapPoint int) (*prog.Prog, int) {
	var tmp []*prog.Prog
	for _, p := range fuzzer.corpus {
		if p.SnapPoint == snapPoint {
			tmp = append(tmp, p)
		}
	}
	if len(tmp) == 0 {
		return nil, 1
	}
	randVal := r.Intn(len(tmp))
	return tmp[randVal], 1
}

func (fuzzer *Fuzzer) addInputToCorpus(p *prog.Prog, sign signal.Signal, sig hash.Sig) {
	fuzzer.corpusMu.Lock()
	if _, ok := fuzzer.corpusHashes[sig]; !ok {
		fuzzer.corpus = append(fuzzer.corpus, p)
		fuzzer.progSignal = append(fuzzer.progSignal, sign)
		fuzzer.corpusHashes[sig] = struct{}{}
		prio := int64(len(sign))
		if sign.Empty() {
			prio = 1
		}
		fuzzer.sumPrios += prio
		fuzzer.corpusPrios = append(fuzzer.corpusPrios, fuzzer.sumPrios)
	}
	fuzzer.corpusMu.Unlock()

	if !sign.Empty() {
		fuzzer.signalMu.Lock()
		fuzzer.corpusSignal.Merge(sign)
		fuzzer.maxSignal.Merge(sign)
		fuzzer.signalMu.Unlock()
	}
}

func (fuzzer *Fuzzer) snapshot() FuzzerSnapshot {
	fuzzer.corpusMu.RLock()
	defer fuzzer.corpusMu.RUnlock()
	return FuzzerSnapshot{fuzzer.corpus, fuzzer.corpusPrios, fuzzer.sumPrios}
}

func (fuzzer *Fuzzer) addMaxSignal(sign signal.Signal) {
	if sign.Len() == 0 {
		return
	}
	fuzzer.signalMu.Lock()
	defer fuzzer.signalMu.Unlock()
	fuzzer.maxSignal.Merge(sign)
}

func (fuzzer *Fuzzer) grabNewSignal() signal.Signal {
	fuzzer.signalMu.Lock()
	defer fuzzer.signalMu.Unlock()
	sign := fuzzer.newSignal
	if sign.Empty() {
		return nil
	}
	fuzzer.newSignal = nil
	return sign
}

func (fuzzer *Fuzzer) corpusSignalDiff(sign signal.Signal) signal.Signal {
	fuzzer.signalMu.RLock()
	defer fuzzer.signalMu.RUnlock()
	return fuzzer.corpusSignal.Diff(sign)
}

func (fuzzer *Fuzzer) checkNewSignal(p *prog.Prog, info *ipc.ProgInfo) (calls []int, extra bool) {
	fuzzer.signalMu.RLock()
	defer fuzzer.signalMu.RUnlock()
	for i, inf := range info.Calls {
		if fuzzer.checkNewCallSignal(p, &inf, i) {
			calls = append(calls, i)
		}
	}
	extra = fuzzer.checkNewCallSignal(p, &info.Extra, -1)
	return
}

func (fuzzer *Fuzzer) checkNewCallSignal(p *prog.Prog, info *ipc.CallInfo, call int) bool {
	diff := fuzzer.maxSignal.DiffRaw(info.Signal, signalPrio(p, info, call))
	if diff.Empty() {
		return false
	}
	fuzzer.signalMu.RUnlock()
	fuzzer.signalMu.Lock()
	fuzzer.maxSignal.Merge(diff)
	fuzzer.newSignal.Merge(diff)
	fuzzer.signalMu.Unlock()
	fuzzer.signalMu.RLock()
	return true
}

func signalPrio(p *prog.Prog, info *ipc.CallInfo, call int) (prio uint8) {
	if call == -1 {
		return 0
	}
	if info.Errno == 0 {
		prio |= 1 << 1
	}
	if !p.Target.CallContainsAny(p.Calls[call]) {
		prio |= 1 << 0
	}
	return
}

func parseOutputType(str string) OutputType {
	switch str {
	case "none":
		return OutputNone
	case "stdout":
		return OutputStdout
	case "dmesg":
		return OutputDmesg
	case "file":
		return OutputFile
	default:
		log.Fatalf("-output flag must be one of none/stdout/dmesg/file")
		return OutputNone
	}
}
