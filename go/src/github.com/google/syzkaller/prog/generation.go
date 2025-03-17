// Copyright 2015 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package prog

import (
	"math/rand"
	"strings"
)

// Generate generates a random program of length ~ncalls.
// calls is a set of allowed syscalls, if nil all syscalls are used.
func (target *Target) Generate(rs rand.Source, ncalls int, ct *ChoiceTable) *Prog {
	p := &Prog{
		Target: target,
	}
	r := newRand(target, rs)
	s := newState(target, ct, nil)

	for len(p.Calls) < ncalls {
		calls := r.generateCall(s, p, len(p.Calls))
		for _, c := range calls {
			s.analyze(c)
			p.Calls = append(p.Calls, c)
		}
	}
	p.debugValidate()
	return p
}

// snapfuzz: Duplicate the 'Generate' function to receive the number of file descriptors
func (target *Target) GenerateWithFD(rs rand.Source, ncalls int, ct *ChoiceTable, fdinfo []int) *Prog {
	p := &Prog{
		Target: target,
		Calls:  make([]*Call, 0),
	}
	r := newRand(target, rs)
	s := newState(target, ct, nil)

	default_meta := target.SyscallMap["syz_get_snapfd"]
	nvidia_meta := target.SyscallMap["syz_get_snapfd$nvidia"]
	mali_meta := target.SyscallMap["syz_get_snapfd$mali"]
	amdgpu_meta := target.SyscallMap["syz_get_snapfd$amdgpu"]

	for i := 0; i < len(fdinfo); i++ {
		var calls []*Call
		if fdinfo[i] == 0 {
			calls = r.generateParticularCall(s, default_meta)
		} else if fdinfo[i] == 1 {
			calls = r.generateParticularCall(s, nvidia_meta)
		} else if fdinfo[i] == 2 {
			calls = r.generateParticularCall(s, mali_meta)
		} else if fdinfo[i] == 3 {
			calls = r.generateParticularCall(s, amdgpu_meta)
		}
		for _, c := range calls {
			s.analyze(c)
			p.Calls = append(p.Calls, c)
		}
	}

	for len(p.Calls) < ncalls {
		calls := r.generateCall(s, p, len(p.Calls))
		for _, c := range calls {
			s.analyze(c)
			p.Calls = append(p.Calls, c)
		}
	}

	for i := len(p.Calls) - 1; i >= len(fdinfo); i-- {
		if strings.Contains(p.Calls[i].Meta.Name, "syz_get_snapfd") {
			p.removeCall(i)
		}
	}

	p.debugValidate()
	return p
}
