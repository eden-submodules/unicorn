/*
 *  emulator main execution loop
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/* Modified for Unicorn Engine by Nguyen Anh Quynh, 2015 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/exec-all.h"
#include "tcg.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "sysemu/sysemu.h"
#include "exec/address-spaces.h"
#include "exec/tb-hash.h"
#include "exec/tb-lookup.h"

#include "uc_priv.h"

/* Execute a TB, and fix up the CPU state afterwards if necessary */
static inline tcg_target_ulong cpu_tb_exec(CPUState *cpu, TranslationBlock *itb)
{
    CPUArchState *env = cpu->env_ptr;
    TCGContext *tcg_ctx = env->uc->tcg_ctx;
    uintptr_t ret;
    TranslationBlock *last_tb;
    int tb_exit;
    uint8_t *tb_ptr = itb->tc.ptr;

    // Unicorn: commented out
    //qemu_log_mask_and_addr(CPU_LOG_EXEC, itb->pc,
    //                       "Trace %p [" TARGET_FMT_lx "] %s\n",
    //                       itb->tc.ptr, itb->pc, lookup_symbol(itb->pc));
    ret = tcg_qemu_tb_exec(env, tb_ptr);
    last_tb = (TranslationBlock *)(ret & ~TB_EXIT_MASK);
    tb_exit = ret & TB_EXIT_MASK;
    //trace_exec_tb_exit(last_tb, tb_exit);

    if (tb_exit > TB_EXIT_IDX1) {
        /* We didn't start executing this TB (eg because the instruction
         * counter hit zero); we must restore the guest PC to the address
         * of the start of the TB.
         */
        CPUClass *cc = CPU_GET_CLASS(env->uc, cpu);
        // Unicorn: commented out
        //qemu_log_mask_and_addr(CPU_LOG_EXEC, last_tb->pc,
        //                       "Stopped execution of TB chain before %p ["
        //                       TARGET_FMT_lx "] %s\n",
        //                       last_tb->tc.ptr, last_tb->pc,
        //                       lookup_symbol(last_tb->pc));
        if (cc->synchronize_from_tb) {
            // avoid sync twice when helper_uc_tracecode() already did this.
            if (env->uc->emu_counter <= env->uc->emu_count &&
                    !env->uc->stop_request && !env->uc->quit_request) {
                cc->synchronize_from_tb(cpu, last_tb);
            }
        } else {
            assert(cc->set_pc);
            // avoid sync twice when helper_uc_tracecode() already did this.
            if (env->uc->emu_counter <= env->uc->emu_count && !env->uc->quit_request) {
                cc->set_pc(cpu, last_tb->pc);
            }
        }
    }
    if (tb_exit == TB_EXIT_REQUESTED) {
        /* We were asked to stop executing TBs (probably a pending
         * interrupt. We've now stopped, so clear the flag.
         */
        atomic_set(&cpu->tcg_exit_req, 0);
    }
    return ret;
}

 /* Execute the code without caching the generated code. An interpreter
    could be used if available. */
static void cpu_exec_nocache(CPUState *cpu, int max_cycles,
                             TranslationBlock *orig_tb, bool ignore_icount)
{
    TranslationBlock *tb;
    CPUArchState *env = (CPUArchState *)cpu->env_ptr;
    uint32_t cflags = curr_cflags(cpu->uc) | CF_NOCACHE;

    if (ignore_icount) {
        cflags &= ~CF_USE_ICOUNT;
    }

    /* Should never happen.
       We only end up here when an existing TB is too long.  */
    cflags |= MIN(max_cycles, CF_COUNT_MASK);

    tb = tb_gen_code(cpu, orig_tb->pc, orig_tb->cs_base,
                     orig_tb->flags, cflags);
    tb->orig_tb = orig_tb;
    /* execute the generated code */
    // Unicorn: commented out
    //trace_exec_tb_nocache(tb, tb->pc);
    cpu_tb_exec(cpu, tb);
    tb_phys_invalidate(env->uc, tb, -1);
    tb_remove(env->uc, tb);
}

struct tb_desc {
    target_ulong pc;
    target_ulong cs_base;
    CPUArchState *env;
    tb_page_addr_t phys_page1;
    uint32_t flags;
    uint32_t cf_mask;
    uint32_t trace_vcpu_dstate;
};

static bool tb_cmp(const void *p, const void *d)
{
    const TranslationBlock *tb = p;
    const struct tb_desc *desc = d;

    if (tb->pc == desc->pc &&
        tb->page_addr[0] == desc->phys_page1 &&
        tb->cs_base == desc->cs_base &&
        tb->flags == desc->flags &&
        tb->trace_vcpu_dstate == desc->trace_vcpu_dstate &&
        (tb_cflags(tb) & (CF_HASH_MASK | CF_INVALID)) == desc->cf_mask) {
        /* check next page if needed */
        if (tb->page_addr[1] == -1) {
            return true;
        } else {
            tb_page_addr_t phys_page2;
            target_ulong virt_page2;

            virt_page2 = (desc->pc & TARGET_PAGE_MASK) + TARGET_PAGE_SIZE;
            phys_page2 = get_page_addr_code(desc->env, virt_page2);
            if (tb->page_addr[1] == phys_page2) {
                return true;
            }
        }
    }
    return false;
}

TranslationBlock *tb_htable_lookup(CPUState *cpu, target_ulong pc,
                                   target_ulong cs_base, uint32_t flags,
                                   uint32_t cf_mask)
{
    tb_page_addr_t phys_pc;
    struct tb_desc desc;
    uint32_t h;

    /* find translated block using physical mappings */
    desc.env = (CPUArchState *)cpu->env_ptr;
    desc.cs_base = cs_base;
    desc.flags = flags;
    desc.cf_mask = cf_mask;
    desc.trace_vcpu_dstate = 0;
    desc.pc = pc;
    phys_pc = get_page_addr_code(desc.env, pc);
    desc.phys_page1 = phys_pc & TARGET_PAGE_MASK;
    h = tb_hash_func(phys_pc, pc, flags, cf_mask, 0);

    return qht_lookup(&cpu->uc->tb_ctx.htable, tb_cmp, &desc, h);
}

void tb_set_jmp_target(TranslationBlock *tb, int n, uintptr_t addr)
{
    if (TCG_TARGET_HAS_direct_jump) {
        uintptr_t offset = tb->jmp_target_arg[n];
        uintptr_t tc_ptr = (uintptr_t)tb->tc.ptr;
        tb_target_set_jmp_target(tc_ptr, tc_ptr + offset, addr);
    } else {
        tb->jmp_target_arg[n] = addr;
    }
}

/* Called with tb_lock held.  */
static inline void tb_add_jump(TranslationBlock *tb, int n,
                               TranslationBlock *tb_next)
{
    assert(n < ARRAY_SIZE(tb->jmp_list_next));
    if (tb->jmp_list_next[n]) {
        /* Another thread has already done this while we were
         * outside of the lock; nothing to do in this case */
        return;
    }
    qemu_log_mask_and_addr(CPU_LOG_EXEC, tb->pc,
                           "Linking TBs %p [" TARGET_FMT_lx
                           "] index %d -> %p [" TARGET_FMT_lx "]\n",
                           tb->tc.ptr, tb->pc, n,
                           tb_next->tc.ptr, tb_next->pc);

    /* patch the native jump address */
    tb_set_jmp_target(tb, n, (uintptr_t)tb_next->tc.ptr);

    /* add in TB jmp circular list */
    tb->jmp_list_next[n] = tb_next->jmp_list_first;
    tb_next->jmp_list_first = (uintptr_t)tb | n;
}

static inline TranslationBlock *tb_find(CPUState *cpu,
                                        TranslationBlock *last_tb,
                                        int tb_exit, uint32_t cf_mask)
{
    TranslationBlock *tb;
    target_ulong cs_base, pc;
    uint32_t flags;
    bool acquired_tb_lock = false;

    tb = tb_lookup__cpu_state(cpu, &pc, &cs_base, &flags, cf_mask);
    if (tb == NULL) {
        /* mmap_lock is needed by tb_gen_code, and mmap_lock must be
         * taken outside tb_lock. As system emulation is currently
         * single threaded the locks are NOPs.
         */
        mmap_lock();
        //tb_lock();
        acquired_tb_lock = true;

        /* There's a chance that our desired tb has been translated while
         * taking the locks so we check again inside the lock.
         */
        tb = tb_htable_lookup(cpu, pc, cs_base, flags, cf_mask);
        if (likely(tb == NULL)) {
            /* if no translated code available, then translate it now */
            tb = tb_gen_code(cpu, pc, cs_base, flags, cf_mask);
        }

        mmap_unlock();

        /* We add the TB in the virtual pc hash table for the fast lookup */
        atomic_set(&cpu->tb_jmp_cache[tb_jmp_cache_hash_func(pc)], tb);
    }
#ifndef CONFIG_USER_ONLY
    /* We don't take care of direct jumps when address mapping changes in
     * system emulation. So it's not safe to make a direct jump to a TB
     * spanning two pages because the mapping for the second page can change.
     */
    if (tb->page_addr[1] != -1) {
        last_tb = NULL;
    }
#endif
    /* See if we can patch the calling TB. */
    if (last_tb && !qemu_loglevel_mask(CPU_LOG_TB_NOCHAIN)) {
        if (!acquired_tb_lock) {
            // Unicorn: commented out
            //tb_lock();
            acquired_tb_lock = true;
        }
        /* Check if translation buffer has been flushed */
        if (cpu->tb_flushed) {
            cpu->tb_flushed = false;
        } else if (!(tb->cflags & CF_INVALID)) {
            tb_add_jump(last_tb, tb_exit, tb);
        }
    }
    if (acquired_tb_lock) {
        // Unicorn: commented out
        //tb_unlock();
    }
    return tb;
}

static inline bool cpu_handle_halt(CPUState *cpu)
{
    if (cpu->halted) {
        if (!cpu_has_work(cpu)) {
            return true;
        }

        cpu->halted = 0;
    }

    return false;
}

static inline void cpu_handle_debug_exception(CPUState *cpu)
{
    CPUClass *cc = CPU_GET_CLASS(cpu->uc, cpu);
    CPUWatchpoint *wp;

    if (!cpu->watchpoint_hit) {
        QTAILQ_FOREACH(wp, &cpu->watchpoints, entry) {
            wp->flags &= ~BP_WATCHPOINT_HIT;
        }
    }

    cc->debug_excp_handler(cpu);
}

static inline bool cpu_handle_exception(struct uc_struct *uc, CPUState *cpu, int *ret)
{
    struct hook *hook;

    if (cpu->exception_index >= 0) {
        if (uc->stop_interrupt && uc->stop_interrupt(cpu->exception_index)) {
            cpu->halted = 1;
            uc->invalid_error = UC_ERR_INSN_INVALID;
            *ret = EXCP_HLT;
            return true;
        }

        if (cpu->exception_index >= EXCP_INTERRUPT) {
            /* exit request from the cpu execution loop */
            *ret = cpu->exception_index;
            if (*ret == EXCP_DEBUG) {
                cpu_handle_debug_exception(cpu);
            }
            cpu->exception_index = -1;
            return true;
        } else {
#if defined(CONFIG_USER_ONLY)
            /* if user mode only, we simulate a fake exception
               which will be handled outside the cpu execution
               loop */
#if defined(TARGET_I386)
            CPUClass *cc = CPU_GET_CLASS(uc, cpu);
            cc->do_interrupt(cpu);
#endif
            *ret = cpu->exception_index;
            cpu->exception_index = -1;
            return true;
#else
            bool catched = false;
            // Unicorn: call registered interrupt callbacks
            HOOK_FOREACH_VAR_DECLARE;
            HOOK_FOREACH(uc, hook, UC_HOOK_INTR) {
                ((uc_cb_hookintr_t)hook->callback)(uc, cpu->exception_index, hook->user_data);
                catched = true;
            }
            // Unicorn: If un-catched interrupt, stop executions.
            if (!catched) {
                cpu->halted = 1;
                uc->invalid_error = UC_ERR_EXCEPTION;
                *ret = EXCP_HLT;
                return true;
            }
            cpu->exception_index = -1;
#endif
        }
    }

    return false;
}

static inline bool cpu_handle_interrupt(CPUState *cpu,
                                        TranslationBlock **last_tb)
{
    CPUClass *cc = CPU_GET_CLASS(cpu->uc, cpu);
    int interrupt_request = cpu->interrupt_request;

    if (unlikely(interrupt_request)) {
        if (unlikely(cpu->singlestep_enabled & SSTEP_NOIRQ)) {
            /* Mask out external interrupts for this step. */
            interrupt_request &= ~CPU_INTERRUPT_SSTEP_MASK;
        }
        if (interrupt_request & CPU_INTERRUPT_DEBUG) {
            cpu->interrupt_request &= ~CPU_INTERRUPT_DEBUG;
            cpu->exception_index = EXCP_DEBUG;
            return true;
        }
        if (interrupt_request & CPU_INTERRUPT_HALT) {
            cpu->interrupt_request &= ~CPU_INTERRUPT_HALT;
            cpu->halted = 1;
            cpu->exception_index = EXCP_HLT;
            return true;
        }
#if defined(TARGET_I386)
        else if (interrupt_request & CPU_INTERRUPT_INIT) {
            X86CPU *x86_cpu = X86_CPU(cpu->uc, cpu);
            CPUArchState *env = &x86_cpu->env;
            cpu_svm_check_intercept_param(env, SVM_EXIT_INIT, 0, 0);
            do_cpu_init(x86_cpu);
            cpu->exception_index = EXCP_HALTED;
            return true;
        }
#else
        else if (interrupt_request & CPU_INTERRUPT_RESET) {
            cpu_reset(cpu);
        }
#endif
        else {
            /* The target hook has 3 exit conditions:
               False when the interrupt isn't processed,
               True when it is, and we should restart on a new TB,
               and via longjmp via cpu_loop_exit.  */
            if (cc->cpu_exec_interrupt(cpu, interrupt_request)) {
                *last_tb = NULL;
            }
            /* The target hook may have updated the 'cpu->interrupt_request';
             * reload the 'interrupt_request' value */
            interrupt_request = cpu->interrupt_request;
        }

        if (interrupt_request & CPU_INTERRUPT_EXITTB) {
            cpu->interrupt_request &= ~CPU_INTERRUPT_EXITTB;
            /* ensure that no TB jump will be modified as
               the program flow was changed */
            *last_tb = NULL;
        }
    }
    if (unlikely(cpu->exit_request)) {
        cpu->exit_request = 0;
        cpu->exception_index = EXCP_INTERRUPT;
        return true;
    }
    return false;
}

static inline void cpu_loop_exec_tb(CPUState *cpu, TranslationBlock *tb,
                                    TranslationBlock **last_tb, int *tb_exit)
{
    uintptr_t ret;

    /* execute the generated code */
    ret = cpu_tb_exec(cpu, tb);
    tb = (TranslationBlock *)(ret & ~TB_EXIT_MASK);
    *tb_exit = ret & TB_EXIT_MASK;
    switch (*tb_exit) {
    case TB_EXIT_REQUESTED:
        /* Something asked us to stop executing chained TBs; just
         * continue round the main loop. Whatever requested the exit
         * will also have set something else (eg interrupt_request)
         * which we will handle next time around the loop.  But we
         * need to ensure the tcg_exit_req read in generated code
         * comes before the next read of cpu->exit_request or
         * cpu->interrupt_request.
         */
        smp_mb();
        *last_tb = NULL;
        break;
    case TB_EXIT_ICOUNT_EXPIRED:
    {
        /* Instruction counter expired.  */
#ifdef CONFIG_USER_ONLY
        abort();
#else
        int insns_left = cpu->icount_decr.u32;
        *last_tb = NULL;
        if (cpu->icount_extra && insns_left >= 0) {
            /* Refill decrementer and continue execution.  */
            cpu->icount_extra += insns_left;
            insns_left = MIN(0xffff, cpu->icount_extra);
            cpu->icount_extra -= insns_left;
            cpu->icount_decr.u16.low = insns_left;
        } else {
            if (insns_left > 0) {
                /* Execute remaining instructions.  */
                cpu_exec_nocache(cpu, insns_left, tb, false);
                // Unicorn: commented out
                //align_clocks(sc, cpu);
            }
            cpu->exception_index = EXCP_INTERRUPT;
            cpu_loop_exit(cpu);
        }
        break;
#endif
    }
    default:
        *last_tb = tb;
        break;
    }
}

void cpu_exec_step_atomic(struct uc_struct *uc, CPUState *cpu)
{
    CPUClass *cc = CPU_GET_CLASS(uc, cpu);
    TranslationBlock *tb;
    target_ulong cs_base, pc;
    uint32_t flags;
    uint32_t cflags = 1;
    uint32_t cf_mask = cflags & CF_HASH_MASK;

    if (sigsetjmp(cpu->jmp_env, 0) == 0) {
        tb = tb_lookup__cpu_state(cpu, &pc, &cs_base, &flags, cf_mask);
        if (tb == NULL) {
            mmap_lock();
            //tb_lock();
            tb = tb_htable_lookup(cpu, pc, cs_base, flags, cf_mask);
            if (likely(tb == NULL)) {
                tb = tb_gen_code(cpu, pc, cs_base, flags, cflags);
            }
            //tb_unlock();
            mmap_unlock();
        }

        // Unicorn: commented out
        //start_exclusive();

        /* Since we got here, we know that parallel_cpus must be true.  */
        uc->parallel_cpus = false;
        cc->cpu_exec_enter(cpu);
        /* execute the generated code */
        cpu_tb_exec(cpu, tb);
        cc->cpu_exec_exit(cpu);
        uc->parallel_cpus = true;

        // Unicorn: commented out
        //end_exclusive()
    } else {
        /* We may have exited due to another problem here, so we need
         * to reset any tb_locks we may have taken but didn't release.
         * The mmap_lock is dropped by tb_gen_code if it runs out of
         * memory.
         */
#ifndef CONFIG_SOFTMMU
        // Unicorn: Commented out
        //tcg_debug_assert(!have_mmap_lock());
#endif
        // Unicorn: commented out
        //tb_lock_reset();
    }
}

/* main execution loop */

int cpu_exec(struct uc_struct *uc, CPUState *cpu)
{
    CPUArchState *env = cpu->env_ptr;
    CPUClass *cc = CPU_GET_CLASS(uc, cpu);
    int ret;

    if (cpu_handle_halt(cpu)) {
        return EXCP_HALTED;
    }

    atomic_mb_set(&uc->current_cpu, cpu);
    atomic_mb_set(&uc->tcg_current_rr_cpu, cpu);

    cc->cpu_exec_enter(cpu);
    cpu->exception_index = -1;
    env->invalid_error = UC_ERR_OK;

    /* prepare setjmp context for exception handling */
    if (sigsetjmp(cpu->jmp_env, 0) != 0) {
#if defined(__clang__) || !QEMU_GNUC_PREREQ(4, 6)
        /* Some compilers wrongly smash all local variables after
         * siglongjmp. There were bug reports for gcc 4.5.0 and clang.
         * Reload essential local variables here for those compilers.
         * Newer versions of gcc would complain about this code (-Wclobbered). */
        cpu = uc->current_cpu;
        env = cpu->env_ptr;
        cc = CPU_GET_CLASS(uc, cpu);
#else /* buggy compiler */
        /* Assert that the compiler does not smash local variables. */
        g_assert(cpu == uc->current_cpu);
        g_assert(cc == CPU_GET_CLASS(uc, cpu));
#endif /* buggy compiler */
        cpu->can_do_io = 1;
        // Unicorn: commented out
        //tb_lock_reset();
    }

    /* if an exception is pending, we execute it here */
    while (!cpu_handle_exception(uc, cpu, &ret)) {
        TranslationBlock *last_tb = NULL;
        int tb_exit = 0;

        while (!cpu_handle_interrupt(cpu, &last_tb)) {
            uint32_t cflags = cpu->cflags_next_tb;
            TranslationBlock *tb;

            /* When requested, use an exact setting for cflags for the next
               execution.  This is used for icount, precise smc, and stop-
               after-access watchpoints.  Since this request should never
               have CF_INVALID set, -1 is a convenient invalid value that
               does not require tcg headers for cpu_common_reset.  */
            if (cflags == -1) {
                cflags = curr_cflags(cpu->uc);
            } else {
                cpu->cflags_next_tb = -1;
            }

            tb = tb_find(cpu, last_tb, tb_exit, cflags);
            if (!tb) {   // invalid TB due to invalid code?
                uc->invalid_error = UC_ERR_FETCH_UNMAPPED;
                ret = EXCP_HLT;
                break;
            }
            cpu_loop_exec_tb(cpu, tb, &last_tb, &tb_exit);
        }
    }

    cc->cpu_exec_exit(cpu);

    // Unicorn: flush JIT cache to because emulation might stop in
    // the middle of translation, thus generate incomplete code.
    // TODO: optimize this for better performance
    tb_flush(cpu);

    return ret;
}
