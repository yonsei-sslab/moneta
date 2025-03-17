// Copyright 2015 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package qemu

import (
	"fmt"
	"io"
	"io/ioutil"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/google/syzkaller/pkg/config"
	"github.com/google/syzkaller/pkg/log"
	"github.com/google/syzkaller/pkg/osutil"
	"github.com/google/syzkaller/prog"
	"github.com/google/syzkaller/sys/targets"
	"github.com/google/syzkaller/vm/vmimpl"
)

const (
	hostAddr = "10.0.2.10"
)

func init() {
	vmimpl.Register("qemu", ctor, true)
}

type Config struct {
	Count    int    `json:"count"`     // number of VMs to run in parallel
	Qemu     string `json:"qemu"`      // qemu binary name (qemu-system-arch by default)
	QemuArgs string `json:"qemu_args"` // additional command line arguments for qemu binary
	// Location of the kernel for injected boot (e.g. arch/x86/boot/bzImage, optional).
	// This is passed to qemu as the -kernel option.
	Kernel string `json:"kernel"`
	// Additional command line options for the booting kernel, for example `root=/dev/sda1`.
	// Can only be specified with kernel.
	Cmdline string `json:"cmdline"`
	Initrd  string `json:"initrd"` // linux initial ramdisk. (optional)
	// qemu image device.
	// The default value "hda" is transformed to "-hda image" for qemu.
	// The modern way of describing qemu hard disks is supported, so the value
	// "drive index=0,media=disk,file=" is transformed to "-drive index=0,media=disk,file=image"
	// for qemu.
	ImageDevice string `json:"image_device"`
	CPU         int    `json:"cpu"`      // number of VM CPUs
	Mem         int    `json:"mem"`      // amount of VM memory in MiB
	Snapshot    bool   `json:"snapshot"` // For building kernels without -snapshot (for pkg/build)

	Agent         int    `json:"agent"`
	LibAgamotto   string `json:"libagamotto"`
	ChkptPoolSize int    `json:"chkpt_pool_size"` // in MiB
}

type Pool struct {
	env        *vmimpl.Env
	cfg        *Config
	target     *targets.Target
	archConfig *archConfig
}

type instance struct {
	cfg         *Config
	target      *targets.Target
	archConfig  *archConfig
	image       string
	debug       bool
	os          string
	workdir     string
	sshkey      string
	sshuser     string
	fuzzerCmd   string
	executorBin string
	ctl_pipe    *os.File
	st_pipe     *os.File
	index       int
	port        int
	rpipe       io.ReadCloser
	wpipe       io.WriteCloser
	qemu        *exec.Cmd
	merger      *vmimpl.OutputMerger
	files       map[string]string
	diagnose    chan bool

	//moneta
	snapPoint  int
	device     string
	experiment bool
}

type archConfig struct {
	Qemu      string
	QemuArgs  string
	TargetDir string
	NicModel  string
	CmdLine   []string
}

var archConfigs = map[string]*archConfig{
	"linux/amd64": {
		Qemu:      "qemu-system-x86_64",
		QemuArgs:  "-enable-kvm -cpu host",
		TargetDir: "/",
		// e1000e fails on recent Debian distros with:
		// Initialization of device e1000e failed: failed to find romfile "efi-e1000e.rom
		// But other arches don't use e1000e, e.g. arm64 uses virtio by default.
		// NicModel: ",model=e1000",
		NicModel: ",model=virtio",
		CmdLine: append(linuxCmdline,
			"root=/dev/sda",
			"console=ttyS0",
			"kvm-intel.nested=1",
			"kvm-intel.unrestricted_guest=1",
			"kvm-intel.vmm_exclusive=1",
			"kvm-intel.fasteoi=1",
			"kvm-intel.ept=1",
			"kvm-intel.flexpriority=1",
			"kvm-intel.vpid=1",
			"kvm-intel.emulate_invalid_guest_state=1",
			"kvm-intel.eptad=1",
			"kvm-intel.enable_shadow_vmcs=1",
			"kvm-intel.pml=1",
			"kvm-intel.enable_apicv=1",
			"dummy_hcd.num=8",
		),
	},
	"linux/386": {
		Qemu:      "qemu-system-i386",
		TargetDir: "/",
		NicModel:  ",model=e1000",
		CmdLine: append(linuxCmdline,
			"root=/dev/sda",
			"console=ttyS0",
		),
	},
	"linux/arm64": {
		Qemu:      "qemu-system-aarch64",
		QemuArgs:  "-machine virt,gic-version=3,its=false -cpu max",
		TargetDir: "/",
		CmdLine: append(linuxCmdline,
			"root=/dev/vda",
			"console=ttyAMA0",
		),
	},
	"linux/arm": {
		Qemu:      "qemu-system-arm",
		TargetDir: "/",
		CmdLine: append(linuxCmdline,
			"root=/dev/vda",
			"console=ttyAMA0",
		),
	},
	"linux/mips64le": {
		Qemu:      "qemu-system-mips64el",
		TargetDir: "/",
		QemuArgs:  "-M malta -cpu MIPS64R2-generic -nodefaults",
		NicModel:  ",model=e1000",
		CmdLine: append(linuxCmdline,
			"root=/dev/sda",
			"console=ttyS0",
		),
	},
	"linux/ppc64le": {
		Qemu:      "qemu-system-ppc64",
		TargetDir: "/",
		QemuArgs:  "-enable-kvm -vga none",
		CmdLine:   linuxCmdline,
	},
	"freebsd/amd64": {
		Qemu:      "qemu-system-x86_64",
		TargetDir: "/",
		QemuArgs:  "-enable-kvm",
		NicModel:  ",model=e1000",
	},
	"netbsd/amd64": {
		Qemu:      "qemu-system-x86_64",
		TargetDir: "/",
		QemuArgs:  "-enable-kvm",
		NicModel:  ",model=e1000",
	},
	"fuchsia/amd64": {
		Qemu:      "qemu-system-x86_64",
		QemuArgs:  "-enable-kvm -machine q35 -cpu host,migratable=off",
		TargetDir: "/tmp",
		NicModel:  ",model=e1000",
		CmdLine: []string{
			"kernel.serial=legacy",
			"kernel.halt-on-panic=true",
		},
	},
	"akaros/amd64": {
		Qemu:      "qemu-system-x86_64",
		QemuArgs:  "-enable-kvm -cpu host,migratable=off",
		TargetDir: "/",
		NicModel:  ",model=e1000",
	},
}

var linuxCmdline = []string{
	"earlyprintk=serial",
	"oops=panic",
	"nmi_watchdog=panic",
	"panic_on_warn=1",
	//"panic=1",
	"ftrace_dump_on_oops=orig_cpu",
	"rodata=n",
	"vsyscall=native",
	"net.ifnames=0",
	"biosdevname=0",
}

func ctor(env *vmimpl.Env) (vmimpl.Pool, error) {
	archConfig := archConfigs[env.OS+"/"+env.Arch]
	cfg := &Config{
		Count:       1,
		CPU:         1,
		ImageDevice: "drive",
		Qemu:        archConfig.Qemu,
		QemuArgs:    archConfig.QemuArgs,
		Snapshot:    false,
	}
	if err := config.LoadData(env.Config, cfg); err != nil {
		return nil, fmt.Errorf("failed to parse qemu vm config: %v", err)
	}
	if cfg.Count < 1 || cfg.Count > 128 {
		return nil, fmt.Errorf("invalid config param count: %v, want [1, 128]", cfg.Count)
	}
	// if env.Debug && cfg.Count > 1 {
	// 	log.Logf(0, "limiting number of VMs from %v to 1 in debug mode", cfg.Count)
	// 	cfg.Count = 1
	// }
	if _, err := exec.LookPath(cfg.Qemu); err != nil {
		return nil, err
	}
	if env.Image == "9p" {
		if env.OS != "linux" {
			return nil, fmt.Errorf("9p image is supported for linux only")
		}
		if cfg.Kernel == "" {
			return nil, fmt.Errorf("9p image requires kernel")
		}
	} else {
		if !osutil.IsExist(env.Image) {
			return nil, fmt.Errorf("image file '%v' does not exist", env.Image)
		}
	}
	if cfg.CPU <= 0 || cfg.CPU > 1024 {
		return nil, fmt.Errorf("bad qemu cpu: %v, want [1-1024]", cfg.CPU)
	}
	if cfg.Mem < 128 || cfg.Mem > 1048576 {
		return nil, fmt.Errorf("bad qemu mem: %v, want [128-1048576]", cfg.Mem)
	}
	cfg.Kernel = osutil.Abs(cfg.Kernel)
	if cfg.ChkptPoolSize < cfg.Mem || cfg.Mem > 1048576 {
		return nil, fmt.Errorf("bad qemu chkpt_pool_size: %v, want [%v-1048576]", cfg.ChkptPoolSize, cfg.Mem)
	}
	cfg.Initrd = osutil.Abs(cfg.Initrd)
	pool := &Pool{
		env:        env,
		cfg:        cfg,
		target:     targets.Get(env.OS, env.Arch),
		archConfig: archConfig,
	}
	return pool, nil
}

func (pool *Pool) Count() int {
	return pool.cfg.Count
}

func (pool *Pool) Create(workdir string, index int) (vmimpl.Instance, error) {
	sshkey := pool.env.SSHKey
	sshuser := pool.env.SSHUser
	if pool.env.Image == "9p" {
		sshkey = filepath.Join(workdir, "key")
		sshuser = "root"
		if _, err := osutil.RunCmd(10*time.Minute, "", "ssh-keygen", "-t", "rsa", "-b", "2048",
			"-N", "", "-C", "", "-f", sshkey); err != nil {
			return nil, err
		}
		initFile := filepath.Join(workdir, "init.sh")
		if err := osutil.WriteExecFile(initFile, []byte(strings.Replace(initScript, "{{KEY}}", sshkey, -1))); err != nil {
			return nil, fmt.Errorf("failed to create init file: %v", err)
		}
	}

	for i := 0; ; i++ {
		inst, err := pool.ctor(workdir, sshkey, sshuser, pool.env.FuzzerCmd, pool.env.ExecutorBin, index)
		if err == nil {
			return inst, nil
		}
		// Older qemu prints "could", newer -- "Could".
		if i < 1000 && strings.Contains(err.Error(), "ould not set up host forwarding rule") {
			continue
		}
		return nil, err
	}
}

func (pool *Pool) ctor(workdir, sshkey, sshuser string, fuzzerCmd string, executorBin string, index int) (vmimpl.Instance, error) {
	inst := &instance{
		cfg:         pool.cfg,
		target:      pool.target,
		archConfig:  pool.archConfig,
		image:       pool.env.Image,
		debug:       pool.env.Debug,
		os:          pool.env.OS,
		workdir:     workdir,
		sshkey:      sshkey,
		sshuser:     sshuser,
		fuzzerCmd:   fuzzerCmd,
		executorBin: executorBin,
		index:       index,
		diagnose:    make(chan bool, 1),
		snapPoint:   pool.env.MatchSnapPoint[index],
		device:      pool.env.SnapShotDevice,
		experiment:  pool.env.SnapShotExperiment,
	}
	if st, err := os.Stat(inst.image); err != nil && st.Size() == 0 {
		// Some kernels may not need an image, however caller may still
		// want to pass us a fake empty image because the rest of syzkaller
		// assumes that an image is mandatory. So if the image is empty, we ignore it.
		inst.image = ""
	}
	closeInst := inst
	defer func() {
		if closeInst != nil {
			closeInst.Close()
		}
	}()

	var err error
	inst.rpipe, inst.wpipe, err = osutil.LongPipe()
	if err != nil {
		return nil, err
	}

	if err := inst.boot(); err != nil {
		return nil, err
	}

	closeInst = nil
	return inst, nil
}

func (inst *instance) Close() {
	if inst.qemu != nil {
		inst.qemu.Process.Kill()
		inst.qemu.Wait()
	}
	if inst.merger != nil {
		inst.merger.Wait()
	}
	if inst.rpipe != nil {
		inst.rpipe.Close()
	}
	if inst.wpipe != nil {
		inst.wpipe.Close()
	}
}

func (inst *instance) boot() error {
	inst.port = vmimpl.UnusedTCPPort()
	var device string

	// Hardcoding for NVIDIA experiment...
	if !inst.experiment {
		device = inst.device
	} else if inst.experiment && inst.device == "nvidia" {
		if 0 <= inst.index && inst.index < 64 {
			device = "nvidia"
		} else if 64 <= inst.index && inst.index < 96 {
			device = "nvidia4060ti"
		} else if 96 <= inst.index && inst.index < 128 {
			device = "nvidia1650"
		}
	}

	args := []string{
		"-m", strconv.Itoa(inst.cfg.Mem),
		"-smp", strconv.Itoa(inst.cfg.CPU),
		"-display", "none",
		"-no-reboot",
		"-monitor", "none", // "telnet::45454,server,nowait",
		"-serial", "mon:stdio",
		"-device", device,
		"-loadvm", fmt.Sprintf("moneta"),
	}
	args = append(args, splitArgs(inst.cfg.QemuArgs, filepath.Join(inst.workdir, "template"))...)
	// PeriScope: debug server
	if inst.debug {
		args = append(args, "-s")
	}
	if inst.image == "9p" {
		args = append(args,
			"-fsdev", "local,id=fsdev0,path=/,security_model=none,readonly",
			"-device", "virtio-9p-pci,fsdev=fsdev0,mount_tag=/dev/root",
		)
	} else if inst.image != "" {
		// inst.cfg.ImageDevice can contain spaces
		imgline := strings.Split(inst.cfg.ImageDevice, " ")
		imgline[0] = "-" + imgline[0]
		if inst.cfg.Count > 1 {
			inst.image = "file=" + inst.image + "_" + strconv.Itoa(inst.index+1) + ",format=qcow2"
		} else {
			inst.image = "file=" + inst.image + ",format=qcow2"
		}
		if strings.HasSuffix(imgline[len(imgline)-1], "file=") {
			imgline[len(imgline)-1] = imgline[len(imgline)-1] + inst.image
		} else {
			imgline = append(imgline, inst.image)
		}
		args = append(args, imgline...)
		if inst.cfg.Snapshot {
			args = append(args, "-snapshot")
		}
	}
	if inst.cfg.Initrd != "" {
		args = append(args,
			"-initrd", inst.cfg.Initrd,
		)
	}
	if inst.cfg.Kernel != "" {
		cmdline := append([]string{}, inst.archConfig.CmdLine...)
		if inst.image == "9p" {
			cmdline = append(cmdline,
				"root=/dev/root",
				"rootfstype=9p",
				"rootflags=trans=virtio,version=9p2000.L,cache=loose",
				"init="+filepath.Join(inst.workdir, "init.sh"),
			)
		}
		cmdline = append(cmdline, inst.cfg.Cmdline)
		args = append(args,
			"-kernel", inst.cfg.Kernel,
			"-append", strings.Join(cmdline, " "),
		)
	}
	if inst.target.VMMFuzzer { // PeriScope
		args = append(args,
			"-object", fmt.Sprintf("memory-backend-file,size=%v,mem-path=/dev/shm/syzkaller-vm%v-in,share,id=mb1", prog.ExecBufferSize, inst.index),
			"-device", fmt.Sprintf("ivshmem-plain,memdev=mb1,master=on"),
			"-object", fmt.Sprintf("memory-backend-file,size=%v,mem-path=/dev/shm/syzkaller-vm%v-out,share,id=mb2", 16<<20, inst.index),
			"-device", fmt.Sprintf("ivshmem-plain,memdev=mb2,master=on,chkpt=on"),
		)
	}

	if inst.debug {
		log.Logf(0, "running command: %v %#v", inst.cfg.Qemu, args)
	}
	qemu := osutil.Command(inst.cfg.Qemu, args...)
	qemu.Stdout = inst.wpipe
	qemu.Stderr = inst.wpipe

	var namedStPipe string
	var namedCtlPipe string
	if inst.target.VMMFuzzer { // PeriScope
		qemu.Env = os.Environ()
		qemu.Env = append(qemu.Env, fmt.Sprintf("LD_PRELOAD=%v", inst.cfg.LibAgamotto))
		agentId := inst.cfg.Agent
		if inst.debug {
			qemu.Env = append(qemu.Env, "SYZ_FUZZER_DEBUG=1")
			agentId = agentId + 1
		}

		tmpDir, _ := ioutil.TempDir("/tmp", "syz")
		namedStPipe = filepath.Join(tmpDir, fmt.Sprintf("vm%v-mgr-0", inst.index))
		namedCtlPipe = filepath.Join(tmpDir, fmt.Sprintf("vm%v-mgr-1", inst.index))
		syscall.Mkfifo(namedStPipe, 0600)
		syscall.Mkfifo(namedCtlPipe, 0600)
		qemu.Env = append(qemu.Env, fmt.Sprintf("SYZ_MANAGER_PIPE=%v", filepath.Join(tmpDir, fmt.Sprintf("vm%v-mgr-N", inst.index))))

		qemu.Env = append(qemu.Env, fmt.Sprintf("__PERISCOPE_CHKPT_POOL_SIZE=%v", inst.cfg.ChkptPoolSize))

		qemu.Env = append(qemu.Env, fmt.Sprintf("__PERISCOPE_GUEST_AGENT_ID=%v", agentId))
		qemu.Env = append(qemu.Env, fmt.Sprintf("SYZ_FUZZER_INDEX=%v", inst.index))
		fuzzerCmd := inst.fuzzerCmd
		fuzzerCmd = strings.Replace(fuzzerCmd, "VMID", fmt.Sprintf("%d", inst.index), 2)
		qemu.Env = append(qemu.Env, fmt.Sprintf("SYZ_FUZZER_PATH=%v", strings.Split(fuzzerCmd, " ")[0]))
		qemu.Env = append(qemu.Env, fmt.Sprintf("SYZ_FUZZER_ARGV=%v", strings.Join(strings.Split(fuzzerCmd, " ")[1:], " ")))

		executorCmd := "/usr/bin/ssh " + strings.Join(vmimpl.SSHArgs(false, inst.sshkey, inst.port), " ") +
			" " + inst.sshuser + "@localhost " + inst.executorBin
		executorCmd += ".wrapper"
		qemu.Env = append(qemu.Env, fmt.Sprintf("SYZ_FUZZER_EXECUTOR=%v", executorCmd))
	}

	if err := qemu.Start(); err != nil {
		return fmt.Errorf("failed to start %v %+v: %v", inst.cfg.Qemu, args, err)
	}
	inst.wpipe.Close()
	inst.wpipe = nil
	inst.qemu = qemu
	// Qemu has started.

	// Start output merger.
	var tee io.Writer
	if inst.debug {
		tee = os.Stdout
	}
	inst.merger = vmimpl.NewOutputMerger(tee)
	inst.merger.Add("qemu", inst.rpipe)
	inst.rpipe = nil

	var bootOutput []byte
	bootOutputStop := make(chan bool)
	go func() {
		for {
			select {
			case out := <-inst.merger.Output:
				bootOutput = append(bootOutput, out...)
			case <-bootOutputStop:
				close(bootOutputStop)
				return
			}
		}
	}()
	// PeriScope
	if inst.target.VMMFuzzer {
		rpipe, err := os.OpenFile(namedStPipe, os.O_RDONLY, 0600)
		if err != nil {
			log.Fatalf("mgr pipe (st) open failed: %v", err)
		}
		inst.st_pipe = rpipe

		wpipe, err := os.OpenFile(namedCtlPipe, os.O_WRONLY, 0600)
		if err != nil {
			log.Fatalf("mgr pipe (ctl) open failed: %v", err)
		}
		inst.ctl_pipe = wpipe

		// Boot event
		var buf [4]byte
		n, err := io.ReadFull(rpipe, buf[:])
		if err != nil {
			log.Fatalf("mgr pipe (st) read failed at boot: %v", err)
		}
		if n != 4 {
			log.Fatalf("mgr pipe returned more than 4 bytes")
		}

		bootOutputStop <- true
		return nil
	}
	if err := vmimpl.WaitForSSH(inst.debug, 10*time.Minute, "localhost",
		inst.sshkey, inst.sshuser, inst.os, inst.port, inst.merger.Err); err != nil {
		bootOutputStop <- true
		<-bootOutputStop
		return vmimpl.MakeBootError(err, bootOutput)
	}
	bootOutputStop <- true
	return nil
}

func splitArgs(str, template string) (args []string) {
	for _, arg := range strings.Split(str, " ") {
		if arg == "" {
			continue
		}
		args = append(args, strings.Replace(arg, "{{TEMPLATE}}", template, -1))
	}
	return
}

func (inst *instance) Forward(port int) (string, error) {
	addr := hostAddr
	if inst.target.HostFuzzer {
		addr = "127.0.0.1"
	}
	return fmt.Sprintf("%v:%v", addr, port), nil
}

func (inst *instance) targetDir() string {
	if inst.image == "9p" {
		return "/tmp"
	}
	return inst.archConfig.TargetDir
}

func (inst *instance) Copy(hostSrc string) (string, error) {
	base := filepath.Base(hostSrc)
	vmDst := filepath.Join(inst.targetDir(), base)
	if inst.target.HostFuzzer {
		if base == "syz-fuzzer" || base == "syz-execprog" {
			return hostSrc, nil // we will run these on host
		}
		if inst.files == nil {
			inst.files = make(map[string]string)
		}
		inst.files[vmDst] = hostSrc
	}

	args := append(vmimpl.SCPArgs(inst.debug, inst.sshkey, inst.port),
		hostSrc, inst.sshuser+"@localhost:"+vmDst)
	if inst.debug {
		log.Logf(0, "running command: scp %#v", args)
	}
	_, err := osutil.RunCmd(3*time.Minute, "", "scp", args...)
	if err != nil {
		return "", err
	}
	return vmDst, nil
}

func (inst *instance) GetPipes() (*os.File, *os.File) {
	return inst.ctl_pipe, inst.st_pipe
}

func (inst *instance) Run(timeout time.Duration, stop <-chan bool, command string) (
	<-chan []byte, <-chan error, error) {
	if inst.target.VMMFuzzer {
		errc := make(chan error, 1)
		signal := func(err error) {
			select {
			case errc <- err:
			default:
			}
		}
		go func() {
		retry:
			select {
			case <-stop:
				signal(vmimpl.ErrTimeout)
			case <-inst.diagnose:
				goto retry
			case err := <-inst.merger.Err:
				signal(err)
				return
			}
		}()
		return inst.merger.Output, errc, nil
	}

	rpipe, wpipe, err := osutil.LongPipe()
	if err != nil {
		return nil, nil, err
	}
	inst.merger.Add("ssh", rpipe)

	sshArgs := vmimpl.SSHArgs(inst.debug, inst.sshkey, inst.port)
	args := strings.Split(command, " ")
	if bin := filepath.Base(args[0]); inst.target.HostFuzzer &&
		(bin == "syz-fuzzer" || bin == "syz-execprog") {
		// Weird mode for akaros.
		// Fuzzer and execprog are on host (we did not copy them), so we will run them as is,
		// but we will also wrap executor with ssh invocation.
		for i, arg := range args {
			if strings.HasPrefix(arg, "-executor=") {
				args[i] = "-executor=" + "/usr/bin/ssh " + strings.Join(sshArgs, " ") +
					" " + inst.sshuser + "@localhost " + arg[len("-executor="):]
			}
			if host := inst.files[arg]; host != "" {
				args[i] = host
			}
		}
	} else {
		args = []string{"ssh"}
		args = append(args, sshArgs...)
		args = append(args, inst.sshuser+"@localhost", "cd "+inst.targetDir()+" && "+command)
	}
	if inst.debug {
		log.Logf(0, "running command: %#v", args)
	}
	cmd := osutil.Command(args[0], args[1:]...)
	cmd.Dir = inst.workdir
	cmd.Stdout = wpipe
	cmd.Stderr = wpipe
	if err := cmd.Start(); err != nil {
		wpipe.Close()
		return nil, nil, err
	}
	wpipe.Close()
	errc := make(chan error, 1)
	signal := func(err error) {
		select {
		case errc <- err:
		default:
		}
	}

	go func() {
	retry:
		select {
		case <-time.After(timeout):
			signal(vmimpl.ErrTimeout)
		case <-stop:
			signal(vmimpl.ErrTimeout)
		case <-inst.diagnose:
			cmd.Process.Kill()
			goto retry
		case err := <-inst.merger.Err:
			cmd.Process.Kill()
			if cmdErr := cmd.Wait(); cmdErr == nil {
				// If the command exited successfully, we got EOF error from merger.
				// But in this case no error has happened and the EOF is expected.
				err = nil
			}
			signal(err)
			return
		}
		cmd.Process.Kill()
		cmd.Wait()
	}()
	return inst.merger.Output, errc, nil
}

func (inst *instance) Diagnose() ([]byte, bool) {
	select {
	case inst.diagnose <- true:
	default:
	}
	return nil, false
}

// nolint: lll
const initScript = `#! /bin/bash
set -eux
mount -t proc none /proc
mount -t sysfs none /sys
mount -t debugfs nodev /sys/kernel/debug/
mount -t tmpfs none /tmp
mount -t tmpfs none /var
mount -t tmpfs none /run
mount -t tmpfs none /etc
mount -t tmpfs none /root
touch /etc/fstab
mkdir /etc/network
mkdir /run/network
printf 'auto lo\niface lo inet loopback\n\n' >> /etc/network/interfaces
printf 'auto eth0\niface eth0 inet static\naddress 10.0.2.15\nnetmask 255.255.255.0\nnetwork 10.0.2.0\ngateway 10.0.2.1\nbroadcast 10.0.2.255\n\n' >> /etc/network/interfaces
printf 'auto eth0\niface eth0 inet6 static\naddress fe80::5054:ff:fe12:3456/64\ngateway 2000:da8:203:612:0:3:0:1\n\n' >> /etc/network/interfaces
mkdir -p /etc/network/if-pre-up.d
mkdir -p /etc/network/if-up.d
ifup lo
ifup eth0 || true
echo "root::0:0:root:/root:/bin/bash" > /etc/passwd
mkdir -p /etc/ssh
cp {{KEY}}.pub /root/
chmod 0700 /root
chmod 0600 /root/key.pub
mkdir -p /var/run/sshd/
chmod 700 /var/run/sshd
groupadd -g 33 sshd
useradd -u 33 -g 33 -c sshd -d / sshd
cat > /etc/ssh/sshd_config <<EOF
          Port 22
          Protocol 2
          UsePrivilegeSeparation no
          HostKey {{KEY}}
          PermitRootLogin yes
          AuthenticationMethods publickey
          ChallengeResponseAuthentication no
          AuthorizedKeysFile /root/key.pub
          IgnoreUserKnownHosts yes
          AllowUsers root
          LogLevel INFO
          TCPKeepAlive yes
          RSAAuthentication yes
          PubkeyAuthentication yes
EOF
/usr/sbin/sshd -e -D
/sbin/halt -f
`
