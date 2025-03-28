#ifndef PERISCOPE_GUEST_PERISCOPE_H
#define PERISCOPE_GUEST_PERISCOPE_H

#include <stdint.h>
#include <stdio.h>

#define KVM_HC_AGAMOTTO 20

#define HC_AGAMOTTO_GET_PROG 0
#define HC_AGAMOTTO_END 1
#define HC_AGAMOTTO_DEBUG 10
#define HC_AGAMOTTO_NEXT 20

enum {
    SYZKALLER_HC_ROOT_CHKPT          = 5,
    SYZKALLER_HC_RECV_EXEC,         // 6
    SYZKALLER_HC_REPLY_EXEC,        // 7
    SYZKALLER_HC_RECV_HANDSHAKE,    // 8
    SYZKALLER_HC_REPLY_HANDSHAKE,   // 9
    SYZKALLER_HC_MAYBE_CHKPT,       // 10
    SYZKALLER_HC_FORKSRV_CTX,       // 11
    AGAMOTTO_DEBUG_HC_BENCHMARK,    // 12
    AGAMOTTO_DEBUG_HC_NEXT,         // 13
    AGAMOTTO_DEBUG_HC_VMSYNC,       // 14
    AGAMOTTO_DEBUG_HC_CLOCK_SCALE,	// 15
	MONETA_DEBUG_HC_SAVEVM,        // 16
	MONETA_DEBUG_HC_CHKCHKPT,      // 17
	MONETA_DEBUG_HC_GPA2HPA,       // 18
};

#ifdef __x86_64__

static uint64_t agamotto_kvm_hypercall(uint64_t a0)
{
	uint64_t ret;
	uint64_t nr = KVM_HC_AGAMOTTO;

	asm("movq %0, %%rbx;"
	    :
	    : "r"(a0));
	asm("movq %0, %%rax;"
	    :
	    : "r"(nr));
#ifdef X86_64_AMD_FAMILY
	asm("vmmcall; movq %% rax,%0"
	    : "=r"(ret)
	    :);
#else
	asm("vmcall; movq %% rax,%0"
	    : "=r"(ret)
	    :);
#endif
	return ret;
}

static uint64_t agamotto_kvm_hypercall2(uint64_t a0, uint64_t a1)
{
	uint64_t ret;
	uint64_t nr = KVM_HC_AGAMOTTO;

	asm("movq %0, %%rcx;"
	    :
	    : "r"(a1));
	asm("movq %0, %%rbx;"
	    :
	    : "r"(a0));
	asm("movq %0, %%rax;"
	    :
	    : "r"(nr));
#ifdef X86_64_AMD_FAMILY
	asm("vmmcall; movq %% rax,%0"
	    : "=r"(ret)
	    :);
#else
	asm("vmcall; movq %% rax,%0"
	    : "=r"(ret)
	    :);
#endif
	return ret;
}

static uint64_t agamotto_kvm_hypercall3(uint64_t a0, uint64_t a1, uint64_t a2)
{
	uint64_t ret;
	uint64_t nr = KVM_HC_AGAMOTTO;

	asm("movq %0, %%rdx;"
	    :
	    : "r"(a2));
	asm("movq %0, %%rcx;"
	    :
	    : "r"(a1));
	asm("movq %0, %%rbx;"
	    :
	    : "r"(a0));
	asm("movq %0, %%rax;"
	    :
	    : "r"(nr));
#ifdef X86_64_AMD_FAMILY
	asm("vmmcall; movq %% rax,%0"
	    : "=r"(ret)
	    :);
#else
	asm("vmcall; movq %% rax,%0"
	    : "=r"(ret)
	    :);
#endif
	return ret;
}

static uint64_t agamotto_kvm_hypercall4(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	uint64_t ret;
	uint64_t nr = KVM_HC_AGAMOTTO;

	asm("movq %0, %%rsi;"
	    :
	    : "r"(a3));
	asm("movq %0, %%rdx;"
	    :
	    : "r"(a2));
	asm("movq %0, %%rcx;"
	    :
	    : "r"(a1));
	asm("movq %0, %%rbx;"
	    :
	    : "r"(a0));
	asm("movq %0, %%rax;"
	    :
	    : "r"(nr));
#ifdef X86_64_AMD_FAMILY
	asm("vmmcall; movq %% rax,%0"
	    : "=r"(ret)
	    :);
#else
	asm("vmcall; movq %% rax,%0"
	    : "=r"(ret)
	    :);
#endif
	return ret;
}

#endif // __x86_64__

static void periscope_agent_exit_cb()
{
	// TODO
	printf("periscope: agent exiting...\n");
}

#endif // PERISCOPE_GUEST_PERISCOPE_H
