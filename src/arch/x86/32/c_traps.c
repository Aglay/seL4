/*
 * Copyright 2014, General Dynamics C4 Systems
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(GD_GPL)
 */

#include <config.h>
#include <model/statedata.h>
#include <arch/machine/fpu.h>
#include <arch/fastpath/fastpath.h>
#include <arch/machine/debug.h>
#include <benchmark/benchmark_track.h>
#include <mode/stack.h>
#include <arch/object/vcpu.h>
#include <arch/kernel/traps.h>

#include <api/syscall.h>
#include <util.h>

#ifdef CONFIG_VTX
static void NORETURN vmlaunch_failed(void)
{
    NODE_LOCK;

    c_entry_hook();

    handleVmEntryFail();
    restore_user_context();
}

static void NORETURN restore_vmx(void)
{
    restoreVMCS();
#ifdef CONFIG_HARDWARE_DEBUG_API
    /* Do not support breakpoints in VMs, so just disable all breakpoints */
    loadAllDisabledBreakpointState(&NODE_STATE(ksCurThread)->tcbArch);
#endif
#if CONFIG_MAX_NUM_NODES > 1
    NODE_STATE(ksCurThread)->tcbArch.vcpu->kernelSP = ((word_t)kernel_stack_alloc[getCurrentCPUIndex()]) + 0xffc;
#endif
    if (NODE_STATE(ksCurThread)->tcbArch.vcpu->launched) {
        /* attempt to do a vmresume */
        asm volatile(
            // Set our stack pointer to the top of the tcb so we can efficiently pop
            "movl %0, %%esp\n"
            "popl %%eax\n"
            "popl %%ebx\n"
            "popl %%ecx\n"
            "popl %%edx\n"
            "popl %%esi\n"
            "popl %%edi\n"
            "popl %%ebp\n"
            // Now do the vmresume
            "vmresume\n"
            // if we get here we failed
            "leal kernel_stack_alloc, %%esp\n"
            "call %1\n"
            :
            : "r"(&NODE_STATE(ksCurThread)->tcbArch.vcpu->gp_registers[EAX]),
            "m"(vmlaunch_failed)
            // Clobber memory so the compiler is forced to complete all stores
            // before running this assembler
            : "memory"
        );
    } else {
        /* attempt to do a vmlaunch */
        asm volatile(
            // Set our stack pointer to the top of the tcb so we can efficiently pop
            "movl %0, %%esp\n"
            "popl %%eax\n"
            "popl %%ebx\n"
            "popl %%ecx\n"
            "popl %%edx\n"
            "popl %%esi\n"
            "popl %%edi\n"
            "popl %%ebp\n"
            // Now do the vmresume
            "vmlaunch\n"
            // if we get here we failed
            "leal kernel_stack_alloc, %%esp\n"
            "call %1\n"
            :
            : "r"(&NODE_STATE(ksCurThread)->tcbArch.vcpu->gp_registers[EAX]),
            "m"(vmlaunch_failed)
            // Clobber memory so the compiler is forced to complete all stores
            // before running this assembler
            : "memory"
        );
    }
    UNREACHABLE();
}
#endif

void NORETURN VISIBLE restore_user_context(void);
void NORETURN VISIBLE restore_user_context(void)
{
    c_exit_hook();

    NODE_UNLOCK_IF_HELD;
#ifdef CONFIG_VTX
    if (thread_state_ptr_get_tsType(&NODE_STATE(ksCurThread)->tcbState) == ThreadState_RunningVM) {
        restore_vmx();
    }
#endif
    setKernelEntryStackPointer(NODE_STATE(ksCurThread));
    if (unlikely(nativeThreadUsingFPU(NODE_STATE(ksCurThread)))) {
        /* We are using the FPU, make sure it is enabled */
        enableFpu();
    } else if (unlikely(ARCH_NODE_STATE(x86KSActiveFPUState))) {
        /* Someone is using the FPU and it might be enabled */
        disableFpu();
    } else {
        /* No-one (including us) is using the FPU, so we assume it
         * is currently disabled */
    }
#ifdef CONFIG_HARDWARE_DEBUG_API
    restore_user_debug_context(NODE_STATE(ksCurThread));
#endif

    /* see if we entered via syscall */
    if (likely(NODE_STATE(ksCurThread)->tcbArch.tcbContext.registers[Error] == -1)) {
        asm volatile(
            // Set our stack pointer to the top of the tcb so we can efficiently pop
            "movl %0, %%esp\n"
            // restore syscall number
            "popl %%eax\n"
            // cap/badge register
            "popl %%ebx\n"
            // skip ecx and edx, these will contain esp and NextIP due to sysenter/sysexit convention
            "addl $8, %%esp\n"
            // message info register
            "popl %%esi\n"
            // message register
            "popl %%edi\n"
            // message register
            "popl %%ebp\n"
            //ds (if changed)
            "cmpl $0x23, (%%esp)\n"
            "je 1f\n"
            "popl %%ds\n"
            "jmp 2f\n"
            "1: addl $4, %%esp\n"
            "2:\n"
            //es (if changed)
            "cmpl $0x23, (%%esp)\n"
            "je 1f\n"
            "popl %%es\n"
            "jmp 2f\n"
            "1: addl $4, %%esp\n"
            "2:\n"
#if defined(CONFIG_FSGSBASE_GDT)
            //have to reload other selectors
            "popl %%fs\n"
            "popl %%gs\n"
            // skip FaultIP, tls_base and error (these are fake registers)
            "addl $12, %%esp\n"
#elif defined(CONFIG_FSGSBASE_MSR)
            /* FS and GS are not touched if MSRs are used */
            "addl $20, %%esp\n"
#else
#error "Invalid method to set IPCBUF/TLS"
#endif
            // restore NextIP
            "popl %%edx\n"
            // skip cs
            "addl $4,  %%esp\n"
            "movl 4(%%esp), %%ecx\n"
            // It is not properly documented but on ia32 (but NOT x86-64) if interrupts
            // are enabled as a result of popf then they will not become enabled till
            // after the next instruction (much like sti)
            // see https://lkml.org/lkml/2004/7/15/6
            "popfl\n"
            "sysexit\n"
            :
            : "r"(NODE_STATE(ksCurThread)->tcbArch.tcbContext.registers)
            // Clobber memory so the compiler is forced to complete all stores
            // before running this assembler
            : "memory"
        );
    } else {
        asm volatile(
            // Set our stack pointer to the top of the tcb so we can efficiently pop
            "movl %0, %%esp\n"
            "popl %%eax\n"
            "popl %%ebx\n"
            "popl %%ecx\n"
            "popl %%edx\n"
            "popl %%esi\n"
            "popl %%edi\n"
            "popl %%ebp\n"
            "popl %%ds\n"
            "popl %%es\n"
#if defined(CONFIG_FSGSBASE_GDT)
            "popl %%fs\n"
            "popl %%gs\n"
            // skip FaultIP, tls_base, error
            "addl $12, %%esp\n"
#elif defined(CONFIG_FSGSBASE_MSR)
            /* skip fs,gs, faultip tls_base, error */
            "addl $20, %%esp\n"
#else
#error "Invalid method to set IPCBUF/TLS"
#endif
            "iret\n"
            :
            : "r"(NODE_STATE(ksCurThread)->tcbArch.tcbContext.registers)
            // Clobber memory so the compiler is forced to complete all stores
            // before running this assembler
            : "memory"
        );
    }

    UNREACHABLE();
}
