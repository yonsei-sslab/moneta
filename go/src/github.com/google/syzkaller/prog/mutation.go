// Copyright 2015 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

package prog

import (
	"fmt"
	"math"
	"math/rand"
	"sort"
	"strings"
	"unsafe"
)

// Maximum length of generated binary blobs inserted into the program.
const maxBlobLen = uint64(100 << 10)

const (
	OpSquash int = iota
	OpSplice
	OpInsCall
	OpMutArg
	OpRmCall
	OpCount
)

// Mutate program p.
//
// p:       The program to mutate.
// rs:      Random source.
// ncalls:  The allowed maximum calls in mutated program.
// ct:      ChoiceTable for syscalls.
// corpus:  The entire corpus, including original program p.
func (p *Prog) Mutate(rs rand.Source, ncalls int, ct *ChoiceTable, corpus []*Prog) (int, int) {
	op := OpRmCall
	idx := 0
	r := newRand(p.Target, rs)
	ctx := &mutator{
		p:      p,
		r:      r,
		ncalls: ncalls,
		ct:     ct,
		corpus: corpus,
		idx:    idx,
	}

	var spIndex int
	for i, c := range p.Calls {
		if strings.Contains(c.Meta.Name, "syz_get_snapfd") {
			spIndex = i
		}
	}
	p.FileDescriptorCount = uint64(spIndex + 1)
	ctx.fds = make([]*Call, p.FileDescriptorCount)
	copy(ctx.fds, ctx.p.Calls)
	ctx.fds = ctx.fds[0:p.FileDescriptorCount]
	ctx.p.Calls = append(p.Calls[:0], p.Calls[p.FileDescriptorCount:]...)

	for stop, ok := false, false; !stop; stop = ok && r.oneOf(3) {
		switch {
		case r.oneOf(5):
			// Not all calls have anything squashable,
			// so this has lower priority in reality.
			ok = ctx.squashAny()
			op = OpSquash
		case r.nOutOf(1, 100):
			ok = ctx.splice()
			op = OpSplice
		case r.nOutOf(20, 31):
			if p.FileDescriptorCount > 0 {
				ok = ctx.insertCallWithFd()
			} else {
				ok = ctx.insertCall()
			}
			op = OpInsCall
		case r.nOutOf(10, 11):
			if p.FileDescriptorCount > 0 {
				ok = ctx.mutateArgFrom()
			} else {
				ok = ctx.mutateArg()
			}
			op = OpMutArg
		default:
			ok = ctx.removeCall()
			op = OpRmCall
		}
	}

	for i := len(p.Calls) - 1; i >= 0; i-- {
		if strings.Contains(p.Calls[i].Meta.Name, "syz_get_snapfd") {
			p.removeCall(i)
		}
	}

	p.Calls = append(ctx.fds, p.Calls...)

	for _, c := range p.Calls {
		p.Target.SanitizeCall(c)
	}
	p.debugValidate()
	return op, ctx.idx
}

// Internal state required for performing mutations -- currently this matches
// the arguments passed to Mutate().
type mutator struct {
	p      *Prog        // The program to mutate.
	r      *randGen     // The randGen instance.
	ncalls int          // The allowed maximum calls in mutated program.
	ct     *ChoiceTable // ChoiceTable for syscalls.
	corpus []*Prog      // The entire corpus, including original program p.
	idx    int          // Mutated from this index

	// moneta
	fds []*Call
}

// This function selects a random other program p0 out of the corpus, and
// mutates ctx.p as follows: preserve ctx.p's Calls up to a random index i
// (exclusive) concatenated with p0's calls from index i (inclusive).
func (ctx *mutator) splice() bool {
	p, r := ctx.p, ctx.r
	if len(ctx.corpus) == 0 || len(p.Calls) == 0 {
		return false
	}
	p0 := ctx.corpus[r.Intn(len(ctx.corpus))]
	p0c := p0.Clone()
	idx := r.Intn(len(p.Calls))
	p.Calls = append(p.Calls[:idx], append(p0c.Calls, p.Calls[idx:]...)...)
	for i := len(p.Calls) - 1; i >= ctx.ncalls; i-- {
		p.removeCall(i)
	}
	ctx.idx = idx + 1
	return true
}

// Picks a random complex pointer and squashes its arguments into an ANY.
// Subsequently, if the ANY contains blobs, mutates a random blob.
func (ctx *mutator) squashAny() bool {
	p, r := ctx.p, ctx.r
	complexPtrs := p.complexPtrs()
	if len(complexPtrs) == 0 {
		return false
	}
	ptr := complexPtrs[r.Intn(len(complexPtrs))]
	callIdx := 0
	for idx, c := range p.Calls {
		ForeachArg(c, func(arg Arg, ctx *ArgCtx) {
			if ptrArg, ok := arg.(*PointerArg); ok && ptrArg == ptr {
				callIdx = idx
				ctx.Stop = true
			}
		})
	}
	if !p.Target.isAnyPtr(ptr.Type()) {
		p.Target.squashPtr(ptr, true)
	}
	var blobs []*DataArg
	var bases []*PointerArg
	ForeachSubArg(ptr, func(arg Arg, ctx *ArgCtx) {
		if data, ok := arg.(*DataArg); ok && arg.Type().Dir() != DirOut {
			blobs = append(blobs, data)
			bases = append(bases, ctx.Base)
		}
	})
	if len(blobs) == 0 {
		return false
	}
	// TODO(dvyukov): we probably want special mutation for ANY.
	// E.g. merging adjacent ANYBLOBs (we don't create them,
	// but they can appear in future); or replacing ANYRES
	// with a blob (and merging it with adjacent blobs).
	idx := r.Intn(len(blobs))
	arg := blobs[idx]
	base := bases[idx]
	baseSize := base.Res.Size()
	arg.data = mutateData(r, arg.Data(), 0, maxBlobLen)
	// Update base pointer if size has increased.
	if baseSize < base.Res.Size() {
		s := analyze(ctx.ct, ctx.corpus, p, p.Calls[0])
		newArg := r.allocAddr(s, base.Type(), base.Res.Size(), base.Res)
		*base = *newArg
	}
	ctx.idx = callIdx
	return true
}

// Inserts a new call at a randomly chosen point (with bias towards the end of
// existing program). Does not insert a call if program already has ncalls.
func (ctx *mutator) insertCall() bool {
	p, r := ctx.p, ctx.r
	if len(p.Calls) >= ctx.ncalls {
		return false
	}
	idx := r.biasedRand(len(p.Calls)+1, 5)
	var c *Call
	if idx < len(p.Calls) {
		c = p.Calls[idx]
	}
	s := analyze(ctx.ct, ctx.corpus, p, c)
	calls := r.generateCall(s, p, idx)
	// TODO: the program might have more than ncalls
	p.insertBefore(c, calls)
	ctx.idx = idx
	return true
}

// moneta
func (ctx *mutator) insertCallWithFd() bool {
	p, r := ctx.p, ctx.r
	fds := ctx.fds

	if len(p.Calls) >= ctx.ncalls {
		return false
	}
	idx := r.biasedRand(len(p.Calls)+1, 5)
	var c *Call
	if idx < len(p.Calls) {
		c = p.Calls[idx]
	}
	s := analyzeWithFd(ctx.ct, ctx.corpus, p, fds, c)
	calls := r.generateCall(s, p, idx)
	// TODO: the program might have more than ncalls
	p.insertBefore(c, calls)
	ctx.idx = idx
	return true
}

// Removes a random call from program.
func (ctx *mutator) removeCall() bool {
	p, r := ctx.p, ctx.r
	if len(p.Calls) == 0 {
		return false
	}
	idx := r.Intn(len(p.Calls))
	p.removeCall(idx)
	return true
}

// Mutate an argument of a random call.
func (ctx *mutator) mutateArg() bool {
	p, r := ctx.p, ctx.r
	if len(p.Calls) == 0 {
		return false
	}

	idx, ok := chooseCall(p, r)
	if !ok {
		return false
	}
	c := p.Calls[idx]
	s := analyze(ctx.ct, ctx.corpus, p, c)
	updateSizes := true
	for stop, ok := false, false; !stop; stop = ok && r.oneOf(3) {
		ok = true
		ma := &mutationArgs{target: p.Target}
		ForeachArg(c, ma.collectArg)
		if len(ma.args) == 0 {
			return false
		}
		chosenIdx := randomChoice(ma.priorities, r)
		arg, ctx := ma.args[chosenIdx], ma.ctxes[chosenIdx]
		calls, ok1 := p.Target.mutateArg(r, s, arg, ctx, &updateSizes)
		if !ok1 {
			ok = false
			continue
		}
		p.insertBefore(c, calls)
		if updateSizes {
			p.Target.assignSizesCall(c)
		}
		p.Target.SanitizeCall(c)
	}
	ctx.idx = idx
	return true
}

// Mutate an argument of a random call.
func (ctx *mutator) mutateArgFrom() bool {
	p, r := ctx.p, ctx.r
	if len(p.Calls) == 0 {
		return false
	}

	idx, ok := chooseCallFrom(p, r, ctx.ct)
	if !ok {
		return false
	}
	c := p.Calls[idx]
	s := analyze(ctx.ct, ctx.corpus, p, c)
	updateSizes := true
	for stop, ok := false, false; !stop; stop = ok && r.oneOf(3) {
		ok = true
		ma := &mutationArgs{target: p.Target}
		ForeachArg(c, ma.collectArg)
		if len(ma.args) == 0 {
			return false
		}
		chosenIdx := randomChoice(ma.priorities, r)
		arg, ctx := ma.args[chosenIdx], ma.ctxes[chosenIdx]
		calls, ok1 := p.Target.mutateArg(r, s, arg, ctx, &updateSizes)
		if !ok1 {
			ok = false
			continue
		}
		p.insertBefore(c, calls)
		if updateSizes {
			p.Target.assignSizesCall(c)
		}
		p.Target.SanitizeCall(c)
	}
	ctx.idx = idx
	return true
}

// Select a call based on the complexity of the arguments.
func chooseCall(p *Prog, r *randGen) (int, bool) {
	var callPriorities []float64
	noArgs := true

	for _, c := range p.Calls {
		totalPrio := float64(0)
		ForeachArg(c, func(arg Arg, ctx *ArgCtx) {
			prio, stopRecursion := arg.Type().getMutationPrio(p.Target, arg, false)
			totalPrio += prio
			ctx.Stop = stopRecursion
		})
		callPriorities = append(callPriorities, totalPrio)
		if len(c.Args) > 0 {
			noArgs = false
		}
	}

	// Calls without arguments.
	if noArgs {
		return -1, false
	}

	return randomChoice(callPriorities, r), true
}

// Select a call based on the complexity of the arguments.
func chooseCallFrom(p *Prog, r *randGen, ct *ChoiceTable) (int, bool) {
	var callPriorities []float64
	noArgs := true
	noEnabled := true

	for _, c := range p.Calls {
		totalPrio := float64(0)

		if ct.enabled[c.Meta] {
			ForeachArg(c, func(arg Arg, ctx *ArgCtx) {
				prio, stopRecursion := arg.Type().getMutationPrio(p.Target, arg, false)
				totalPrio += prio
				ctx.Stop = stopRecursion
			})
			// Requires a call other than syz_snap_fd* to be enabled.
			if !strings.Contains(c.Meta.Name, "syz_get_snapfd") {
				noEnabled = false
			}
		}
		callPriorities = append(callPriorities, totalPrio)
		if len(c.Args) > 0 {
			noArgs = false
		}
	}

	// Calls without arguments.
	if noArgs || noEnabled {
		return -1, false
	}

	return randomChoice(callPriorities, r), true
}

// Generate a random index from a given 1-D array of priorities.
func randomChoice(priorities []float64, r *randGen) int {
	sum := float64(0)
	probs := make([]float64, len(priorities))
	for i, prio := range priorities {
		sum += prio
		probs[i] = sum
	}
	return sort.SearchFloat64s(probs, sum*r.Float64())
}

func (target *Target) mutateArg(r *randGen, s *state, arg Arg, ctx ArgCtx, updateSizes *bool) ([]*Call, bool) {
	var baseSize uint64
	if ctx.Base != nil {
		baseSize = ctx.Base.Res.Size()
	}
	calls, retry, preserve := arg.Type().mutate(r, s, arg, ctx)
	if retry {
		return nil, false
	}
	if preserve {
		*updateSizes = false
	}
	// Update base pointer if size has increased.
	if base := ctx.Base; base != nil && baseSize < base.Res.Size() {
		newArg := r.allocAddr(s, base.Type(), base.Res.Size(), base.Res)
		replaceArg(base, newArg)
	}
	for _, c := range calls {
		target.SanitizeCall(c)
	}
	return calls, true
}

func regenerate(r *randGen, s *state, arg Arg) (calls []*Call, retry, preserve bool) {
	var newArg Arg
	newArg, calls = r.generateArg(s, arg.Type())
	replaceArg(arg, newArg)
	return
}

func mutateInt(r *randGen, a *ConstArg, t *IntType) uint64 {
	switch {
	case r.nOutOf(1, 3):
		return a.Val + (uint64(r.Intn(4)) + 1)
	case r.nOutOf(1, 2):
		return a.Val - (uint64(r.Intn(4)) + 1)
	default:
		return a.Val ^ (1 << uint64(r.Intn(int(t.TypeBitSize()))))
	}
}

func mutateAlignedInt(r *randGen, a *ConstArg, t *IntType) uint64 {
	rangeEnd := t.RangeEnd
	if t.RangeBegin == 0 && int64(rangeEnd) == -1 {
		// Special [0:-1] range for all possible values.
		rangeEnd = uint64(1<<t.TypeBitSize() - 1)
	}
	index := (a.Val - t.RangeBegin) / t.Align
	misalignment := (a.Val - t.RangeBegin) % t.Align
	switch {
	case r.nOutOf(1, 3):
		index += uint64(r.Intn(4)) + 1
	case r.nOutOf(1, 2):
		index -= uint64(r.Intn(4)) + 1
	default:
		index ^= 1 << uint64(r.Intn(int(t.TypeBitSize())))
	}
	lastIndex := (rangeEnd - t.RangeBegin) / t.Align
	index %= lastIndex + 1
	return t.RangeBegin + index*t.Align + misalignment
}

func (t *IntType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	if r.bin() {
		return regenerate(r, s, arg)
	}
	a := arg.(*ConstArg)
	if t.Align == 0 {
		a.Val = mutateInt(r, a, t)
	} else {
		a.Val = mutateAlignedInt(r, a, t)
	}
	a.Val = truncateToBitSize(a.Val, t.TypeBitSize())
	return
}

func (t *FlagsType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	a := arg.(*ConstArg)
	for oldVal := a.Val; oldVal == a.Val; {
		a.Val = r.flags(t.Vals, t.BitMask, a.Val)
	}
	return
}

func (t *LenType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	if !r.mutateSize(arg.(*ConstArg), *ctx.Parent) {
		retry = true
		return
	}
	preserve = true
	return
}

func (t *ResourceType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	return regenerate(r, s, arg)
}

func (t *VmaType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	return regenerate(r, s, arg)
}

func (t *ProcType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	return regenerate(r, s, arg)
}

func (t *BufferType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	minLen, maxLen := uint64(0), maxBlobLen
	if t.Kind == BufferBlobRange {
		minLen, maxLen = t.RangeBegin, t.RangeEnd
	}
	a := arg.(*DataArg)
	if t.Dir() == DirOut {
		mutateBufferSize(r, a, minLen, maxLen)
		return
	}
	switch t.Kind {
	case BufferBlobRand, BufferBlobRange:
		data := append([]byte{}, a.Data()...)
		a.data = mutateData(r, data, minLen, maxLen)
	case BufferString:
		if len(t.Values) != 0 {
			a.data = r.randString(s, t)
		} else {
			if t.TypeSize != 0 {
				minLen, maxLen = t.TypeSize, t.TypeSize
			}
			data := append([]byte{}, a.Data()...)
			a.data = mutateData(r, data, minLen, maxLen)
		}
	case BufferFilename:
		a.data = []byte(r.filename(s, t))
	case BufferText:
		data := append([]byte{}, a.Data()...)
		a.data = r.mutateText(t.Text, data)
	default:
		panic("unknown buffer kind")
	}
	return
}

func mutateBufferSize(r *randGen, arg *DataArg, minLen, maxLen uint64) {
	for oldSize := arg.Size(); oldSize == arg.Size(); {
		arg.size += uint64(r.Intn(33)) - 16
		if arg.size < minLen {
			arg.size = minLen
		}
		if arg.size > maxLen {
			arg.size = maxLen
		}
	}
}

func (t *ArrayType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	// TODO: swap elements of the array
	a := arg.(*GroupArg)
	count := uint64(0)
	switch t.Kind {
	case ArrayRandLen:
		if r.bin() {
			for count = uint64(len(a.Inner)); r.bin(); {
				count++
			}
		} else {
			for count == uint64(len(a.Inner)) {
				count = r.randArrayLen()
			}
		}
	case ArrayRangeLen:
		if t.RangeBegin == t.RangeEnd {
			panic("trying to mutate fixed length array")
		}
		for count == uint64(len(a.Inner)) {
			count = r.randRange(t.RangeBegin, t.RangeEnd)
		}
	}
	if count > uint64(len(a.Inner)) {
		for count > uint64(len(a.Inner)) {
			newArg, newCalls := r.generateArg(s, t.Type)
			a.Inner = append(a.Inner, newArg)
			calls = append(calls, newCalls...)
			for _, c := range newCalls {
				s.analyze(c)
			}
		}
	} else if count < uint64(len(a.Inner)) {
		for _, arg := range a.Inner[count:] {
			removeArg(arg)
		}
		a.Inner = a.Inner[:count]
	}
	return
}

func (t *PtrType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	a := arg.(*PointerArg)
	if r.oneOf(1000) {
		removeArg(a.Res)
		index := r.rand(len(r.target.SpecialPointers))
		newArg := MakeSpecialPointerArg(t, index)
		replaceArg(arg, newArg)
		return
	}
	newArg := r.allocAddr(s, t, a.Res.Size(), a.Res)
	replaceArg(arg, newArg)
	return
}

func (t *StructType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	gen := r.target.SpecialTypes[t.Name()]
	if gen == nil {
		panic("bad arg returned by mutationArgs: StructType")
	}
	var newArg Arg
	newArg, calls = gen(&Gen{r, s}, t, arg)
	a := arg.(*GroupArg)
	for i, f := range newArg.(*GroupArg).Inner {
		replaceArg(a.Inner[i], f)
	}
	return
}

func (t *UnionType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	if gen := r.target.SpecialTypes[t.Name()]; gen != nil {
		var newArg Arg
		newArg, calls = gen(&Gen{r, s}, t, arg)
		replaceArg(arg, newArg)
	} else {
		a := arg.(*UnionArg)
		current := -1
		for i, option := range t.Fields {
			if a.Option.Type().FieldName() == option.FieldName() {
				current = i
				break
			}
		}
		if current == -1 {
			panic("can't find current option in union")
		}
		newIdx := r.Intn(len(t.Fields) - 1)
		if newIdx >= current {
			newIdx++
		}
		optType := t.Fields[newIdx]
		removeArg(a.Option)
		var newOpt Arg
		newOpt, calls = r.generateArg(s, optType)
		replaceArg(arg, MakeUnionArg(t, newOpt))
	}
	return
}

func (t *CsumType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	panic("CsumType can't be mutated")
}

func (t *ConstType) mutate(r *randGen, s *state, arg Arg, ctx ArgCtx) (calls []*Call, retry, preserve bool) {
	panic("ConstType can't be mutated")
}

type mutationArgs struct {
	target        *Target
	args          []Arg
	ctxes         []ArgCtx
	priorities    []float64
	ignoreSpecial bool
}

const (
	maxPriority = float64(10)
	minPriority = float64(1)
	dontMutate  = float64(0)
)

func (ma *mutationArgs) collectArg(arg Arg, ctx *ArgCtx) {
	ignoreSpecial := ma.ignoreSpecial
	ma.ignoreSpecial = false

	typ := arg.Type()
	prio, stopRecursion := typ.getMutationPrio(ma.target, arg, ignoreSpecial)
	ctx.Stop = stopRecursion

	if prio == dontMutate {
		return
	}

	_, isArrayTyp := typ.(*ArrayType)
	_, isBufferTyp := typ.(*BufferType)
	if !isBufferTyp && !isArrayTyp && typ.Dir() == DirOut || !typ.Varlen() && typ.Size() == 0 {
		return
	}

	ma.args = append(ma.args, arg)
	ma.ctxes = append(ma.ctxes, *ctx)
	ma.priorities = append(ma.priorities, prio)
}

// TODO: find a way to estimate optimal priority values.
// Assign a priority for each type. The boolean is the reference type and it has
// the minimum priority, since it has only two possible values.
func (t *IntType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	// For a integer without a range of values, the priority is based on
	// the number of bits occupied by the underlying type.
	plainPrio := math.Log2(float64(t.TypeBitSize())) + 0.1*maxPriority
	if t.Kind != IntRange {
		return plainPrio, false
	}

	size := t.RangeEnd - t.RangeBegin + 1
	if t.Align != 0 {
		if t.RangeBegin == 0 && int64(t.RangeEnd) == -1 {
			// Special [0:-1] range for all possible values.
			size = (1<<t.TypeBitSize()-1)/t.Align + 1
		} else {
			size = (t.RangeEnd-t.RangeBegin)/t.Align + 1
		}
	}
	switch {
	case size <= 15:
		// For a small range, we assume that it is effectively
		// similar with FlagsType and we need to try all possible values.
		prio = rangeSizePrio(size)
	case size <= 256:
		// We consider that a relevant range has at most 256
		// values (the number of values that can be represented on a byte).
		prio = maxPriority
	default:
		// Ranges larger than 256 are equivalent with a plain integer.
		prio = plainPrio
	}
	return prio, false
}

func (t *StructType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	if target.SpecialTypes[t.Name()] == nil || ignoreSpecial {
		return dontMutate, false
	}
	return maxPriority, true
}

func (t *UnionType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	if target.SpecialTypes[t.Name()] == nil && len(t.Fields) == 1 || ignoreSpecial {
		return dontMutate, false
	}
	// For a non-special type union with more than one option
	// we mutate the union itself and also the value of the current option.
	if target.SpecialTypes[t.Name()] == nil {
		return maxPriority, false
	}
	return maxPriority, true
}

func (t *FlagsType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	prio = rangeSizePrio(uint64(len(t.Vals)))
	if t.BitMask {
		// We want a higher priority because the mutation will include
		// more possible operations (bitwise operations).
		prio += 0.1 * maxPriority
	}
	return prio, false
}

// Assigns a priority based on the range size.
func rangeSizePrio(size uint64) (prio float64) {
	switch size {
	case 0:
		prio = dontMutate
	case 1:
		prio = minPriority
	default:
		// Priority proportional with the number of values. After a threshold, the priority is constant.
		// The threshold is 15 because most of the calls have <= 15 possible values for a flag.
		prio = math.Min(float64(size)/3+0.4*maxPriority, 0.9*maxPriority)
	}
	return prio
}

func (t *PtrType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	if arg.(*PointerArg).IsSpecial() {
		// TODO: we ought to mutate this, but we don't have code for this yet.
		return dontMutate, false
	}
	return 0.3 * maxPriority, false
}

func (t *ConstType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	return dontMutate, false
}

func (t *CsumType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	return dontMutate, false
}

func (t *ProcType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	return 0.5 * maxPriority, false
}

func (t *ResourceType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	return 0.5 * maxPriority, false
}

func (t *VmaType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	return 0.5 * maxPriority, false
}

func (t *LenType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	return 0.6 * maxPriority, false
}

func (t *BufferType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	if t.Dir() == DirOut && !t.Varlen() {
		return dontMutate, false
	}
	if t.Kind == BufferString && len(t.Values) == 1 {
		// These are effectively consts (and frequently file names).
		return dontMutate, false
	}
	return 0.8 * maxPriority, false
}

func (t *ArrayType) getMutationPrio(target *Target, arg Arg, ignoreSpecial bool) (prio float64, stopRecursion bool) {
	if t.Kind == ArrayRangeLen && t.RangeBegin == t.RangeEnd {
		return dontMutate, false
	}
	return maxPriority, false
}

func mutateData(r *randGen, data []byte, minLen, maxLen uint64) []byte {
	for stop := false; !stop; stop = stop && r.oneOf(3) {
		f := mutateDataFuncs[r.Intn(len(mutateDataFuncs))]
		data, stop = f(r, data, minLen, maxLen)
	}
	return data
}

// The maximum delta for integer mutations.
const maxDelta = 35

var mutateDataFuncs = [...]func(r *randGen, data []byte, minLen, maxLen uint64) ([]byte, bool){
	// TODO(dvyukov): duplicate part of data.
	// Flip bit in byte.
	func(r *randGen, data []byte, minLen, maxLen uint64) ([]byte, bool) {
		if len(data) == 0 {
			return data, false
		}
		byt := r.Intn(len(data))
		bit := r.Intn(8)
		data[byt] ^= 1 << uint(bit)
		return data, true
	},
	// Insert random bytes.
	func(r *randGen, data []byte, minLen, maxLen uint64) ([]byte, bool) {
		if len(data) == 0 || uint64(len(data)) >= maxLen {
			return data, false
		}
		n := r.Intn(16) + 1
		if r := int(maxLen) - len(data); n > r {
			n = r
		}
		pos := r.Intn(len(data))
		for i := 0; i < n; i++ {
			data = append(data, 0)
		}
		copy(data[pos+n:], data[pos:])
		for i := 0; i < n; i++ {
			data[pos+i] = byte(r.Int31())
		}
		if uint64(len(data)) > maxLen || r.bin() {
			data = data[:len(data)-n] // preserve original length
		}
		return data, true
	},
	// Remove bytes.
	func(r *randGen, data []byte, minLen, maxLen uint64) ([]byte, bool) {
		if len(data) == 0 {
			return data, false
		}
		n := r.Intn(16) + 1
		if n > len(data) {
			n = len(data)
		}
		pos := 0
		if n < len(data) {
			pos = r.Intn(len(data) - n)
		}
		copy(data[pos:], data[pos+n:])
		data = data[:len(data)-n]
		if uint64(len(data)) < minLen || r.bin() {
			for i := 0; i < n; i++ {
				data = append(data, 0) // preserve original length
			}
		}
		return data, true
	},
	// Append a bunch of bytes.
	func(r *randGen, data []byte, minLen, maxLen uint64) ([]byte, bool) {
		if uint64(len(data)) >= maxLen {
			return data, false
		}
		const max = 256
		n := max - r.biasedRand(max, 10)
		if r := int(maxLen) - len(data); n > r {
			n = r
		}
		for i := 0; i < n; i++ {
			data = append(data, byte(r.rand(256)))
		}
		return data, true
	},
	// Replace int8/int16/int32/int64 with a random value.
	func(r *randGen, data []byte, minLen, maxLen uint64) ([]byte, bool) {
		width := 1 << uint(r.Intn(4))
		if len(data) < width {
			return data, false
		}
		i := r.Intn(len(data) - width + 1)
		storeInt(data[i:], r.Uint64(), width)
		return data, true
	},
	// Add/subtract from an int8/int16/int32/int64.
	func(r *randGen, data []byte, minLen, maxLen uint64) ([]byte, bool) {
		width := 1 << uint(r.Intn(4))
		if len(data) < width {
			return data, false
		}
		i := r.Intn(len(data) - width + 1)
		v := loadInt(data[i:], width)
		delta := r.rand(2*maxDelta+1) - maxDelta
		if delta == 0 {
			delta = 1
		}
		if r.oneOf(10) {
			v = swapInt(v, width)
			v += delta
			v = swapInt(v, width)
		} else {
			v += delta
		}
		storeInt(data[i:], v, width)
		return data, true
	},
	// Set int8/int16/int32/int64 to an interesting value.
	func(r *randGen, data []byte, minLen, maxLen uint64) ([]byte, bool) {
		width := 1 << uint(r.Intn(4))
		if len(data) < width {
			return data, false
		}
		i := r.Intn(len(data) - width + 1)
		value := r.randInt64()
		if r.oneOf(10) {
			value = swap64(value)
		}
		storeInt(data[i:], value, width)
		return data, true
	},
}

func swap16(v uint16) uint16 {
	v0 := byte(v >> 0)
	v1 := byte(v >> 8)
	v = 0
	v |= uint16(v1) << 0
	v |= uint16(v0) << 8
	return v
}

func swap32(v uint32) uint32 {
	v0 := byte(v >> 0)
	v1 := byte(v >> 8)
	v2 := byte(v >> 16)
	v3 := byte(v >> 24)
	v = 0
	v |= uint32(v3) << 0
	v |= uint32(v2) << 8
	v |= uint32(v1) << 16
	v |= uint32(v0) << 24
	return v
}

func swap64(v uint64) uint64 {
	v0 := byte(v >> 0)
	v1 := byte(v >> 8)
	v2 := byte(v >> 16)
	v3 := byte(v >> 24)
	v4 := byte(v >> 32)
	v5 := byte(v >> 40)
	v6 := byte(v >> 48)
	v7 := byte(v >> 56)
	v = 0
	v |= uint64(v7) << 0
	v |= uint64(v6) << 8
	v |= uint64(v5) << 16
	v |= uint64(v4) << 24
	v |= uint64(v3) << 32
	v |= uint64(v2) << 40
	v |= uint64(v1) << 48
	v |= uint64(v0) << 56
	return v
}

func swapInt(v uint64, size int) uint64 {
	switch size {
	case 1:
		return v
	case 2:
		return uint64(swap16(uint16(v)))
	case 4:
		return uint64(swap32(uint32(v)))
	case 8:
		return swap64(v)
	default:
		panic(fmt.Sprintf("swapInt: bad size %v", size))
	}
}

func loadInt(data []byte, size int) uint64 {
	p := unsafe.Pointer(&data[0])
	switch size {
	case 1:
		return uint64(*(*uint8)(p))
	case 2:
		return uint64(*(*uint16)(p))
	case 4:
		return uint64(*(*uint32)(p))
	case 8:
		return *(*uint64)(p)
	default:
		panic(fmt.Sprintf("loadInt: bad size %v", size))
	}
}

func storeInt(data []byte, v uint64, size int) {
	p := unsafe.Pointer(&data[0])
	switch size {
	case 1:
		*(*uint8)(p) = uint8(v)
	case 2:
		*(*uint16)(p) = uint16(v)
	case 4:
		*(*uint32)(p) = uint32(v)
	case 8:
		*(*uint64)(p) = v
	default:
		panic(fmt.Sprintf("storeInt: bad size %v", size))
	}
}
