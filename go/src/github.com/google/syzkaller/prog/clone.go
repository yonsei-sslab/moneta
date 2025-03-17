// Copyright 2017 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package prog

import (
	"fmt"
	"sort"
	"strings"
)

func (p *Prog) Clone() *Prog {
	p1 := &Prog{
		Target:              p.Target,
		Calls:               make([]*Call, len(p.Calls)),
		FileDescriptorCount: p.FileDescriptorCount,
		SnapPoint:           p.SnapPoint,
	}
	newargs := make(map[*ResultArg]*ResultArg)
	for ci, c := range p.Calls {
		c1 := new(Call)
		c1.Meta = c.Meta
		c1.Begin = c.Begin
		if c.Ret != nil {
			c1.Ret = clone(c.Ret, newargs).(*ResultArg)
		}
		c1.Args = make([]Arg, len(c.Args))
		for ai, arg := range c.Args {
			c1.Args[ai] = clone(arg, newargs)
		}
		p1.Calls[ci] = c1
	}
	p1.debugValidate()
	return p1
}

func clone(arg Arg, newargs map[*ResultArg]*ResultArg) Arg {
	var arg1 Arg
	switch a := arg.(type) {
	case *ConstArg:
		a1 := new(ConstArg)
		*a1 = *a
		arg1 = a1
	case *PointerArg:
		a1 := new(PointerArg)
		*a1 = *a
		arg1 = a1
		if a.Res != nil {
			a1.Res = clone(a.Res, newargs)
		}
	case *DataArg:
		a1 := new(DataArg)
		*a1 = *a
		a1.data = append([]byte{}, a.data...)
		arg1 = a1
	case *GroupArg:
		a1 := new(GroupArg)
		*a1 = *a
		arg1 = a1
		a1.Inner = make([]Arg, len(a.Inner))
		for i, arg2 := range a.Inner {
			a1.Inner[i] = clone(arg2, newargs)
		}
	case *UnionArg:
		a1 := new(UnionArg)
		*a1 = *a
		arg1 = a1
		a1.Option = clone(a.Option, newargs)
	case *ResultArg:
		a1 := new(ResultArg)
		*a1 = *a
		arg1 = a1
		if a1.Res != nil {
			r := newargs[a1.Res]
			a1.Res = r
			if r.uses == nil {
				r.uses = make(map[*ResultArg]bool)
			}
			r.uses[a1] = true
		}
		a1.uses = nil // filled when we clone the referent
		newargs[a] = a1
	default:
		panic(fmt.Sprintf("bad arg kind: %#v", arg))
	}
	return arg1
}

func (p *Prog) SnapClone(fdmap map[uint64]uint64, isgpu map[uint64]int) *Prog {
	target := p.Target

	newargs := make(map[*ResultArg]*ResultArg)
	fdargs := make(map[*ResultArg]int)
	fdkey := make([]int, 0)

	// Insert syz_snap_mmap at beginning of the program
	syz_snap_mmap_meta := target.SyscallMap["syz_snap_mmap"]
	mmapcalls := make([]*Call, 0)

	for _, mmap := range p.MmapInfo {
		var c *Call
		c = &Call{
			Meta: syz_snap_mmap_meta,
			Ret:  MakeReturnArg(syz_snap_mmap_meta.Ret),
		}
		// fmt.Printf("Insert #%v %v -> syz_snapmmap\n", i, mmap.Meta.Name)
		for _, arg := range mmap.Args {
			c.Args = append(c.Args, arg)
		}
		mmapcalls = append(mmapcalls, c)
	}

	p.Calls = append(mmapcalls, p.Calls...)

	// Remove mmap and munmap
	for i := len(p.Calls) - 1; i >= 0; i-- {
		if p.Calls[i].Meta.Name != "syz_snap_mmap" && (strings.Contains(p.Calls[i].Meta.Name, "mmap") || strings.Contains(p.Calls[i].Meta.Name, "munmap")) {
			// fmt.Printf("Remove %v #%v\n", p.Calls[i].Meta.Name, i)
			p.RemoveCall(i)
		}
	}

	p1 := &Prog{
		Target:              p.Target,
		Calls:               make([]*Call, len(p.Calls)),
		FileDescriptorCount: p.FileDescriptorCount,
	}

	for ci, c := range p.Calls {
		c1 := new(Call)
		c1.Meta = c.Meta
		c1.Begin = c.Begin
		if c.Ret != nil {
			c1.Ret = snapclone(c.Ret, newargs, fdargs, fdmap).(*ResultArg)
		}
		c1.Args = make([]Arg, len(c.Args))
		for ai, arg := range c.Args {
			c1.Args[ai] = snapclone(arg, newargs, fdargs, fdmap)
		}
		p1.Calls[ci] = c1
	}

	for _, k := range fdmap {
		fdkey = append(fdkey, int(k))
	}
	sort.Ints(fdkey)

	default_meta := target.SyscallMap["syz_get_snapfd"]
	nvidia_meta := target.SyscallMap["syz_get_snapfd$nvidia"]
	mali_meta := target.SyscallMap["syz_get_snapfd$mali"]
	amdgpu_meta := target.SyscallMap["syz_get_snapfd$amdgpu"]
	fdcalls := make([]*Call, 0)

	for _, k := range fdkey {
		var c *Call
		fmt.Printf("%v -> %v\n", k, isgpu[uint64(k)])
		if isgpu[uint64(k)] == 1 {
			c = &Call{
				Meta: nvidia_meta,
				Ret:  MakeReturnArg(nvidia_meta.Ret),
			}
		} else if isgpu[uint64(k)] == 2 {
			c = &Call{
				Meta: mali_meta,
				Ret:  MakeReturnArg(mali_meta.Ret),
			}
		} else if isgpu[uint64(k)] == 3 {
			c = &Call{
				Meta: amdgpu_meta,
				Ret:  MakeReturnArg(amdgpu_meta.Ret),
			}
		} else {
			c = &Call{
				Meta: default_meta,
				Ret:  MakeReturnArg(default_meta.Ret),
			}
		}
		fdcalls = append(fdcalls, c)
	}
	p1.Calls = append(fdcalls, p1.Calls...)

	for _, c := range p1.Calls {
		for _, arg := range c.Args {
			insertSyzfd(p1, &arg, newargs, fdargs, fdkey, fdmap)
		}
	}

	// Now set fd of syz_snap_mmap
	// Maybe syz_get_snapfd being used is matching to dev file...
	fd_found := false
	var dev_ret *ResultArg
	for _, c := range p1.Calls {
		if !fd_found && strings.Contains(c.Meta.Name, "syz_get_snapfd$") && c.Ret.uses != nil {
			dev_ret = c.Ret
			fd_found = true
		}

		if fd_found && strings.Contains(c.Meta.Name, "syz_snap_mmap") {
			(c.Args[4]).(*ResultArg).Res = dev_ret
			dev_ret.uses[(c.Args[4]).(*ResultArg)] = true
		}
	}

	p1.debugValidate()
	return p1
}

func snapclone(arg Arg, newargs map[*ResultArg]*ResultArg, fdargs map[*ResultArg]int, fdmap map[uint64]uint64) Arg {
	var arg1 Arg
	switch a := arg.(type) {
	case *ConstArg:
		a1 := new(ConstArg)
		*a1 = *a
		arg1 = a1
	case *PointerArg:
		a1 := new(PointerArg)
		*a1 = *a
		arg1 = a1
		if a.Res != nil {
			a1.Res = snapclone(a.Res, newargs, fdargs, fdmap)
		}
	case *DataArg:
		a1 := new(DataArg)
		*a1 = *a
		a1.data = append([]byte{}, a.data...)
		arg1 = a1
	case *GroupArg:
		a1 := new(GroupArg)
		*a1 = *a
		arg1 = a1
		a1.Inner = make([]Arg, len(a.Inner))
		for i, arg2 := range a.Inner {
			a1.Inner[i] = snapclone(arg2, newargs, fdargs, fdmap)
		}
	case *UnionArg:
		a1 := new(UnionArg)
		*a1 = *a
		arg1 = a1
		a1.Option = snapclone(a.Option, newargs, fdargs, fdmap)
	case *ResultArg:
		a1 := new(ResultArg)
		*a1 = *a
		arg1 = a1
		if a1.Res != nil {
			r := newargs[a1.Res]
			if r != nil {
				// case1: Resources opened after the snapshot
				a1.Res = r
				if r.uses == nil {
					r.uses = make(map[*ResultArg]bool)
				}
				r.uses[a1] = true
			} else {
				if _, ok := fdmap[a.Val]; ok {
					// case2: Resources opened before the snapshot and still open
					if fdargs[a1.Res] == 0 {
						fdargs[a1.Res] = int(fdmap[a.Val])
					}
				} else {
					// case3: Resources opened before the snapshot and currently closed
					a1.Res = nil
				}
			}
		}

		// if fdmap[a.Val] != 0 {
		// 	a1.Val = fdmap[a.Val]
		// }

		a1.uses = nil // filled when we clone the referent
		newargs[a] = a1
	default:
		panic(fmt.Sprintf("bad arg kind: %#v", arg))
	}
	return arg1
}

func insertSyzfd(p *Prog, arg *Arg, newargs map[*ResultArg]*ResultArg, fdargs map[*ResultArg]int, fdkey []int, fdmap map[uint64]uint64) {
	switch a := (*arg).(type) {
	case *ConstArg:
	case *PointerArg:
		if a.Res != nil {
			insertSyzfd(p, &a.Res, newargs, fdargs, fdkey, fdmap)
		}
	case *DataArg:
	case *GroupArg:
		for _, arg2 := range a.Inner {
			insertSyzfd(p, &arg2, newargs, fdargs, fdkey, fdmap)
		}
	case *UnionArg:
		insertSyzfd(p, &a.Option, newargs, fdargs, fdkey, fdmap)
	case *ResultArg:
		if a.Res != nil {
			if fdargs[a.Res] > 0 {
				idx := findIndex(fdkey, fdargs[a.Res])
				a.Res = p.Calls[idx].Ret
				if p.Calls[idx].Ret.uses == nil {
					p.Calls[idx].Ret.uses = make(map[*ResultArg]bool)
				}
				p.Calls[idx].Ret.uses[a] = true
			}
		} else if fdmap[a.Val] != 0 {
			idx := findIndex(fdkey, int(fdmap[a.Val]))
			if idx != -1 {
				a.Res = p.Calls[idx].Ret
				if p.Calls[idx].Ret.uses == nil {
					p.Calls[idx].Ret.uses = make(map[*ResultArg]bool)
				}
				p.Calls[idx].Ret.uses[a] = true

			}
		}

	default:
		panic(fmt.Sprintf("bad arg kind: %#v", arg))
	}
}

func findIndex(slice []int, val int) int {
	for i, v := range slice {
		if v == val {
			return i
		}
	}

	return -1
}
