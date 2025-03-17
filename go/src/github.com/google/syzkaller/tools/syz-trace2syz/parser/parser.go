// Copyright 2018 syzkaller project authors. All rights reserved.
// Use of this source code is governed by Apache 2 LICENSE that can be found in the LICENSE file.

//go:build !codeanalysis
// +build !codeanalysis

package parser

import (
	"bufio"
	"bytes"
	"fmt"
	"strconv"
	"strings"
	"time"

	"github.com/google/syzkaller/pkg/log"
)

func parseSyscall(scanner []byte) (int, *Syscall) {
	lex := newStraceLexer(scanner)
	ret := StraceParse(lex)
	return ret, lex.result
}

func shouldSkip(line string) bool {
	return strings.Contains(line, "ERESTART") ||
		strings.Contains(line, "+++") ||
		strings.Contains(line, "---") ||
		strings.Contains(line, "<ptrace(SYSCALL):No such process>")
}

// ParseLoop parses each line of a strace file in a loop.
func ParseData(data []byte) (*TraceTree, error) {
	tree := NewTraceTree()
	// Creating the process tree
	scanner := bufio.NewScanner(bytes.NewReader(data))
	scanner.Buffer(nil, 64<<20)
	for scanner.Scan() {
		line := scanner.Text()
		fields := strings.Fields(line)
		beginTime := fields[0]

		if _, err := strconv.Atoi(beginTime); err == nil {
			// field 0 is proc id
			beginTime = fields[1]
		}

		duration := fields[len(fields)-1]
		if duration[0:1] == "<" {
			duration = duration[1 : len(duration)-2]
		} else {
			duration = "0.0"
		}

		line = strings.Replace(line, beginTime, "", 1)
		scanner_byte := []byte(line)

		if shouldSkip(line) {
			continue
		}
		log.Logf(4, "scanning call: %s", line)
		ret, call := parseSyscall(scanner_byte)
		if call == nil || ret != 0 {
			fmt.Printf("failed to parse line: %v\n", line)
			continue
		}
		call.Begin = beginTime
		str := "2014-09-12T" + beginTime + "Z"
		t, err := time.Parse(time.RFC3339Nano, str)
		if err != nil {
		}
		call.Begin = t.Round(time.Microsecond).UTC().Format("15:04:05.000000")
		duration_ns, _ := strconv.Atoi(strings.Replace(duration, ".", "", 1))
		t = t.Add(time.Nanosecond * time.Duration(duration_ns))
		call.End = t.Round(time.Microsecond).UTC().Format("15:04:05.000000")

		tree.add(call)
	}
	if err := scanner.Err(); err != nil {
		return nil, err
	}
	if len(tree.TraceMap) == 0 {
		return nil, nil
	}
	return tree, nil
}
