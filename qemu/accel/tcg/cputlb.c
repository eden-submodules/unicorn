/*
 *  Common CPU TLB handling
 *
 *  Copyright (c) 2003 Fabrice Bellard
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
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "exec/cpu_ldst.h"
#include "exec/cputlb.h"
#include "exec/memory-internal.h"
#include "exec/ram_addr.h"
#include "tcg/tcg.h"
#include "exec/helper-proto.h"
#include "qemu/atomic.h"

#include "uc_priv.h"

/* DEBUG defines, enable DEBUG_TLB_LOG to log to the CPU_LOG_MMU target */
/* #define DEBUG_TLB */
/* #define DEBUG_TLB_LOG */

#ifdef DEBUG_TLB
# define DEBUG_TLB_GATE 1
# ifdef DEBUG_TLB_LOG
#  define DEBUG_TLB_LOG_GATE 1
# else
#  define DEBUG_TLB_LOG_GATE 0
# endif
#else
# define DEBUG_TLB_GATE 0
# define DEBUG_TLB_LOG_GATE 0
#endif

#define tlb_debug(fmt, ...) do { \
    if (DEBUG_TLB_LOG_GATE) { \
        qemu_log_mask(CPU_LOG_MMU, "%s: " fmt, __func__, \
                      ## __VA_ARGS__); \
    } else if (DEBUG_TLB_GATE) { \
        fprintf(stderr, "%s: " fmt, __func__, ## __VA_ARGS__); \
    } \
} while (0)

static void tlb_flush_entry(CPUTLBEntry *tlb_entry, target_ulong addr);
static bool tlb_is_dirty_ram(CPUTLBEntry *tlbe);
static ram_addr_t qemu_ram_addr_from_host_nofail(struct uc_struct *uc, void *ptr);
static void tlb_add_large_page(CPUArchState *env, target_ulong vaddr,
                               target_ulong size);
static void tlb_set_dirty1(CPUTLBEntry *tlb_entry, target_ulong vaddr);

/* This is OK because CPU architectures generally permit an
 * implementation to drop entries from the TLB at any time, so
 * flushing more entries than required is only an efficiency issue,
 * not a correctness issue.
 */
void tlb_flush(CPUState *cpu)
{
    CPUArchState *env = cpu->env_ptr;

    memset(env->tlb_table, -1, sizeof(env->tlb_table));
    memset(env->tlb_v_table, -1, sizeof(env->tlb_v_table));
    cpu_tb_jmp_cache_clear(cpu);

    env->vtlb_index = 0;
    env->tlb_flush_addr = -1;
    env->tlb_flush_mask = 0;
}

void tlb_flush_page(CPUState *cpu, target_ulong addr)
{
    CPUArchState *env = cpu->env_ptr;
    int i;
    int mmu_idx;

    tlb_debug("page :" TARGET_FMT_lx "\n", addr);

    /* Check if we need to flush due to large pages.  */
    if ((addr & env->tlb_flush_mask) == env->tlb_flush_addr) {
        tlb_debug("forcing full flush ("
                  TARGET_FMT_lx "/" TARGET_FMT_lx ")\n",
                  env->tlb_flush_addr, env->tlb_flush_mask);

        tlb_flush(cpu);
        return;
    }

    addr &= TARGET_PAGE_MASK;
    i = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        tlb_flush_entry(&env->tlb_table[mmu_idx][i], addr);
    }

    /* check whether there are entries that need to be flushed in the vtlb */
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        int k;
        for (k = 0; k < CPU_VTLB_SIZE; k++) {
            tlb_flush_entry(&env->tlb_v_table[mmu_idx][k], addr);
        }
    }

    tb_flush_jmp_cache(cpu, addr);
}

void tlb_reset_dirty_range(CPUTLBEntry *tlb_entry, uintptr_t start,
                           uintptr_t length)
{
    uintptr_t addr;

    if (tlb_is_dirty_ram(tlb_entry)) {
        addr = (tlb_entry->addr_write & TARGET_PAGE_MASK) + tlb_entry->addend;
        if ((addr - start) < length) {
            tlb_entry->addr_write |= TLB_NOTDIRTY;
        }
    }
}

void tlb_reset_dirty(CPUState *cpu, ram_addr_t start1, ram_addr_t length)
{
    CPUArchState *env;

    int mmu_idx;

    env = cpu->env_ptr;
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        unsigned int i;

        for (i = 0; i < CPU_TLB_SIZE; i++) {
            tlb_reset_dirty_range(&env->tlb_table[mmu_idx][i],
                                  start1, length);
        }

        for (i = 0; i < CPU_VTLB_SIZE; i++) {
            tlb_reset_dirty_range(&env->tlb_v_table[mmu_idx][i],
                                  start1, length);
        }
    }
}

/* update the TLB corresponding to virtual page vaddr
   so that it is no longer dirty */
void tlb_set_dirty(CPUState *cpu, target_ulong vaddr)
{
    CPUArchState *env = cpu->env_ptr;
    int i;
    int mmu_idx;

    vaddr &= TARGET_PAGE_MASK;
    i = (vaddr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        tlb_set_dirty1(&env->tlb_table[mmu_idx][i], vaddr);
    }

    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        int k;
        for (k = 0; k < CPU_VTLB_SIZE; k++) {
            tlb_set_dirty1(&env->tlb_v_table[mmu_idx][k], vaddr);
        }
    }
}


/* Add a new TLB entry. At most one entry for a given virtual address
   is permitted. Only a single TARGET_PAGE_SIZE region is mapped, the
   supplied size is only used by tlb_flush_page.  */
void tlb_set_page_with_attrs(CPUState *cpu, target_ulong vaddr,
                             hwaddr paddr, MemTxAttrs attrs, int prot,
                             int mmu_idx, target_ulong size)
{
    CPUArchState *env = cpu->env_ptr;
    MemoryRegionSection *section;
    unsigned int index;
    target_ulong address;
    target_ulong code_address;
    uintptr_t addend;
    CPUTLBEntry *te;
    hwaddr iotlb, xlat, sz;
    unsigned vidx = env->vtlb_index++ % CPU_VTLB_SIZE;
    int asidx = cpu_asidx_from_attrs(cpu, attrs);

    assert(size >= TARGET_PAGE_SIZE);
    if (size != TARGET_PAGE_SIZE) {
        tlb_add_large_page(env, vaddr, size);
    }

    sz = size;
    section = address_space_translate_for_iotlb(cpu, asidx, paddr, &xlat, &sz);
    assert(sz >= TARGET_PAGE_SIZE);

    tlb_debug("vaddr=" TARGET_FMT_lx " paddr=0x" TARGET_FMT_plx
              " prot=%x idx=%d\n",
              vaddr, paddr, prot, mmu_idx);

    address = vaddr;
    if (!memory_region_is_ram(section->mr) && !memory_region_is_romd(section->mr)) {
        /* IO memory case */
        address |= TLB_MMIO;
        addend = 0;
    } else {
        /* TLB_MMIO for rom/romd handled below */
        addend = (uintptr_t)((char*)memory_region_get_ram_ptr(section->mr) + xlat);
    }

    code_address = address;
    iotlb = memory_region_section_get_iotlb(cpu, section, vaddr, paddr, xlat,
                                            prot, &address);

    index = (vaddr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    te = &env->tlb_table[mmu_idx][index];

    /* do not discard the translation in te, evict it into a victim tlb */
    env->tlb_v_table[mmu_idx][vidx] = *te;
    env->iotlb_v[mmu_idx][vidx] = env->iotlb[mmu_idx][index];

    /* refill the tlb */
    env->iotlb[mmu_idx][index].addr = iotlb - vaddr;
    env->iotlb[mmu_idx][index].attrs = attrs;
    te->addend = (uintptr_t)(addend - vaddr);
    if (prot & PAGE_READ) {
        te->addr_read = address;
    } else {
        te->addr_read = -1;
    }

    if (prot & PAGE_EXEC) {
        te->addr_code = code_address;
    } else {
        te->addr_code = -1;
    }
    if (prot & PAGE_WRITE) {
        if ((memory_region_is_ram(section->mr) && section->readonly)
            || memory_region_is_romd(section->mr)) {
            /* Write access calls the I/O callback.  */
            te->addr_write = address | TLB_MMIO;
        } else if (memory_region_is_ram(section->mr)) {
            te->addr_write = address | TLB_NOTDIRTY;
        } else {
            te->addr_write = address;
        }
    } else {
        te->addr_write = -1;
    }
}

/* Add a new TLB entry, but without specifying the memory
 * transaction attributes to be used.
 */
void tlb_set_page(CPUState *cpu, target_ulong vaddr,
                  hwaddr paddr, int prot,
                  int mmu_idx, target_ulong size)
{
    tlb_set_page_with_attrs(cpu, vaddr, paddr, MEMTXATTRS_UNSPECIFIED,
                            prot, mmu_idx, size);
}

static ram_addr_t qemu_ram_addr_from_host_nofail(struct uc_struct *uc, void *ptr)
{
    ram_addr_t ram_addr;

    ram_addr = qemu_ram_addr_from_host(uc, ptr);
    if (ram_addr == RAM_ADDR_INVALID) {
        //error_report("Bad ram pointer %p", ptr);
        return RAM_ADDR_INVALID;
    }

    return ram_addr;
}

/* NOTE: this function can trigger an exception */
/* NOTE2: the returned address is not exactly the physical address: it
 * is actually a ram_addr_t (in system mode; the user mode emulation
 * version of this function returns a guest virtual address).
 */
tb_page_addr_t get_page_addr_code(CPUArchState *env, target_ulong addr)
{
    int mmu_idx, index, pd;
    void *p;
    MemoryRegion *mr;
    ram_addr_t  ram_addr;
    CPUState *cpu = ENV_GET_CPU(env);
    CPUIOTLBEntry *iotlbentry;
    hwaddr physaddr;

    index = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    mmu_idx = cpu_mmu_index(env, true);
    if (unlikely(env->tlb_table[mmu_idx][index].addr_code !=
                 (addr & TARGET_PAGE_MASK))) {
        cpu_ldub_code(env, addr);
        //check for NX related error from softmmu
        if (env->invalid_error == UC_ERR_FETCH_PROT) {
            return RAM_ADDR_INVALID;
        }
    }
    iotlbentry = &env->iotlb[mmu_idx][index];
    pd = iotlbentry->addr & ~TARGET_PAGE_MASK;
    mr = iotlb_to_region(cpu, pd, iotlbentry->attrs);
    if (memory_region_is_unassigned(cpu->uc, mr)) {
        /* Give the new-style cpu_transaction_failed() hook first chance
         * to handle this.
         * This is not the ideal place to detect and generate CPU
         * exceptions for instruction fetch failure (for instance
         * we don't know the length of the access that the CPU would
         * use, and it would be better to go ahead and try the access
         * and use the MemTXResult it produced). However it is the
         * simplest place we have currently available for the check.
         */
        physaddr = (iotlbentry->addr & TARGET_PAGE_MASK) + addr;
        cpu_transaction_failed(cpu, physaddr, addr, 0, MMU_INST_FETCH, mmu_idx,
                               iotlbentry->attrs, MEMTX_DECODE_ERROR, 0);

        cpu_unassigned_access(cpu, addr, false, true, 0, 4);
        /* The CPU's unassigned access hook might have longjumped out
         * with an exception. If it didn't (or there was no hook) then
         * we can't proceed further.
         */
        env->invalid_addr = addr;
        env->invalid_error = UC_ERR_FETCH_UNMAPPED;
        return RAM_ADDR_INVALID;
    }
    p = (void *)((uintptr_t)addr + env->tlb_table[mmu_idx][index].addend);
    ram_addr = qemu_ram_addr_from_host_nofail(cpu->uc, p);
    if (ram_addr == RAM_ADDR_INVALID) {
        env->invalid_addr = addr;
        env->invalid_error = UC_ERR_FETCH_UNMAPPED;
        return RAM_ADDR_INVALID;
    } else {
        return ram_addr;
    }
}

static void tlb_set_dirty1(CPUTLBEntry *tlb_entry, target_ulong vaddr)
{
    if (tlb_entry->addr_write == (vaddr | TLB_NOTDIRTY)) {
        tlb_entry->addr_write = vaddr;
    }
}

/* Our TLB does not support large pages, so remember the area covered by
   large pages and trigger a full TLB flush if these are invalidated.  */
static void tlb_add_large_page(CPUArchState *env, target_ulong vaddr,
                               target_ulong size)
{
    target_ulong mask = ~(size - 1);

    if (env->tlb_flush_addr == (target_ulong)-1) {
        env->tlb_flush_addr = vaddr & mask;
        env->tlb_flush_mask = mask;
        return;
    }
    /* Extend the existing region to include the new page.
       This is a compromise between unnecessary flushes and the cost
       of maintaining a full variable size TLB.  */
    mask &= env->tlb_flush_mask;
    while (((env->tlb_flush_addr ^ vaddr) & mask) != 0) {
        mask <<= 1;
    }
    env->tlb_flush_addr &= mask;
    env->tlb_flush_mask = mask;
}

static bool tlb_is_dirty_ram(CPUTLBEntry *tlbe)
{
    return (tlbe->addr_write & (TLB_INVALID_MASK|TLB_MMIO|TLB_NOTDIRTY)) == 0;
}

static inline void v_tlb_flush_by_mmuidx(CPUState *cpu, uint16_t idxmap)
{
    CPUArchState *env = cpu->env_ptr;
    unsigned long mmu_idx_bitmask = idxmap;
    int mmu_idx;

    tlb_debug("start\n");

    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        if (test_bit(mmu_idx, &mmu_idx_bitmask)) {
            tlb_debug("%d\n", mmu_idx);

            memset(env->tlb_table[mmu_idx], -1, sizeof(env->tlb_table[0]));
            memset(env->tlb_v_table[mmu_idx], -1, sizeof(env->tlb_v_table[0]));
        }
    }

    cpu_tb_jmp_cache_clear(cpu);
}

void tlb_flush_by_mmuidx(CPUState *cpu, uint16_t idxmap)
{
    v_tlb_flush_by_mmuidx(cpu, idxmap);
}

static inline void tlb_flush_entry(CPUTLBEntry *tlb_entry, target_ulong addr)
{
    if (addr == (tlb_entry->addr_read &
                 (TARGET_PAGE_MASK | TLB_INVALID_MASK)) ||
        addr == (tlb_entry->addr_write &
                 (TARGET_PAGE_MASK | TLB_INVALID_MASK)) ||
        addr == (tlb_entry->addr_code &
                 (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {
        memset(tlb_entry, -1, sizeof(*tlb_entry));
    }
}

void tlb_flush_page_by_mmuidx(CPUState *cpu, target_ulong addr, uint16_t idxmap)
{
    CPUArchState *env = cpu->env_ptr;
    unsigned long mmu_idx_bitmap = idxmap;
    int i, page, mmu_idx;

    tlb_debug("addr "TARGET_FMT_lx"\n", addr);

    /* Check if we need to flush due to large pages.  */
    if ((addr & env->tlb_flush_mask) == env->tlb_flush_addr) {
        tlb_debug("forced full flush ("
                  TARGET_FMT_lx "/" TARGET_FMT_lx ")\n",
                  env->tlb_flush_addr, env->tlb_flush_mask);

        v_tlb_flush_by_mmuidx(cpu, idxmap);
        return;
    }

    addr &= TARGET_PAGE_MASK;
    page = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    for (mmu_idx = 0; mmu_idx < NB_MMU_MODES; mmu_idx++) {
        if (test_bit(mmu_idx, &mmu_idx_bitmap)) {
            tlb_flush_entry(&env->tlb_table[mmu_idx][page], addr);
            /* check whether there are vltb entries that need to be flushed */
            for (i = 0; i < CPU_VTLB_SIZE; i++) {
                tlb_flush_entry(&env->tlb_v_table[mmu_idx][i], addr);
            }
        }
    }

    tb_flush_jmp_cache(cpu, addr);
}

static uint64_t io_readx(CPUArchState *env, CPUIOTLBEntry *iotlbentry,
                         int mmu_idx,
                         target_ulong addr, uintptr_t retaddr, int size)
{
    CPUState *cpu = ENV_GET_CPU(env);
    hwaddr physaddr = iotlbentry->addr;
    MemoryRegion *mr = iotlb_to_region(cpu, physaddr, iotlbentry->attrs);
    uint64_t val;
    MemTxResult r;

    physaddr = (physaddr & TARGET_PAGE_MASK) + addr;
    cpu->mem_io_pc = retaddr;
    if (mr != &cpu->uc->io_mem_rom && mr != &cpu->uc->io_mem_notdirty && !cpu->can_do_io) {
        cpu_io_recompile(cpu, retaddr);
    }

    cpu->mem_io_vaddr = addr;
    r = memory_region_dispatch_read(mr, physaddr,
                                    &val, size, iotlbentry->attrs);
    if (r != MEMTX_OK) {
        cpu_transaction_failed(cpu, physaddr, addr, size, MMU_DATA_LOAD,
                               mmu_idx, iotlbentry->attrs, r, retaddr);
    }
    return val;
}

static void io_writex(CPUArchState *env, CPUIOTLBEntry *iotlbentry,
                      int mmu_idx,
                      uint64_t val, target_ulong addr,
                      uintptr_t retaddr, int size)
{
    CPUState *cpu = ENV_GET_CPU(env);
    hwaddr physaddr = iotlbentry->addr;
    MemoryRegion *mr = iotlb_to_region(cpu, physaddr, iotlbentry->attrs);
    MemTxResult r;

    physaddr = (physaddr & TARGET_PAGE_MASK) + addr;
    if (mr != &cpu->uc->io_mem_rom && mr != &cpu->uc->io_mem_notdirty && !cpu->can_do_io) {
        cpu_io_recompile(cpu, retaddr);
    }

    cpu->mem_io_vaddr = addr;
    cpu->mem_io_pc = retaddr;
    r = memory_region_dispatch_write(mr, physaddr,
                                     val, size, iotlbentry->attrs);
    if (r != MEMTX_OK) {
        cpu_transaction_failed(cpu, physaddr, addr, size, MMU_DATA_STORE,
                               mmu_idx, iotlbentry->attrs, r, retaddr);
    }
}

/* Return true if ADDR is present in the victim tlb, and has been copied
   back to the main tlb.  */
static bool victim_tlb_hit(CPUArchState *env, size_t mmu_idx, size_t index,
                           size_t elt_ofs, target_ulong page)
{
    size_t vidx;
    for (vidx = 0; vidx < CPU_VTLB_SIZE; ++vidx) {
        CPUTLBEntry *vtlb = &env->tlb_v_table[mmu_idx][vidx];
        target_ulong cmp = *(target_ulong *)((uintptr_t)vtlb + elt_ofs);

        if (cmp == page) {
            /* Found entry in victim tlb, swap tlb and iotlb.  */
            CPUTLBEntry tmptlb, *tlb = &env->tlb_table[mmu_idx][index];
            CPUIOTLBEntry tmpio, *io = &env->iotlb[mmu_idx][index];
            CPUIOTLBEntry *vio = &env->iotlb_v[mmu_idx][vidx];

            tmptlb = *tlb; *tlb = *vtlb; *vtlb = tmptlb;
            tmpio = *io; *io = *vio; *vio = tmpio;
            return true;
        }
    }
    return false;
}

/* Macro to call the above, with local variables from the use context.  */
#define VICTIM_TLB_HIT(TY, ADDR) \
    victim_tlb_hit(env, mmu_idx, index, offsetof(CPUTLBEntry, TY), \
                 (ADDR) & TARGET_PAGE_MASK)

/* Probe for whether the specified guest write access is permitted.
 * If it is not permitted then an exception will be taken in the same
 * way as if this were a real write access (and we will not return).
 * Otherwise the function will return, and there will be a valid
 * entry in the TLB for this access.
 */
void probe_write(CPUArchState *env, target_ulong addr, int size, int mmu_idx,
                 uintptr_t retaddr)
{
    int index = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    target_ulong tlb_addr = env->tlb_table[mmu_idx][index].addr_write;

    if ((addr & TARGET_PAGE_MASK)
        != (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {
        /* TLB entry is for a different page */
        if (!VICTIM_TLB_HIT(addr_write, addr)) {
            tlb_fill(ENV_GET_CPU(env), addr, size, MMU_DATA_STORE,
                     mmu_idx, retaddr);
        }
    }
}

/* Probe for a read-modify-write atomic operation.  Do not allow unaligned
 * operations, or io operations to proceed.  Return the host address.  */
static void *atomic_mmu_lookup(CPUArchState *env, target_ulong addr,
                               TCGMemOpIdx oi, uintptr_t retaddr)
{
    size_t mmu_idx = get_mmuidx(oi);
    size_t index = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    CPUTLBEntry *tlbe = &env->tlb_table[mmu_idx][index];
    target_ulong tlb_addr = tlbe->addr_write;
    TCGMemOp mop = get_memop(oi);
    int a_bits = get_alignment_bits(mop);
    int s_bits = mop & MO_SIZE;

    /* Adjust the given return address.  */
    retaddr -= GETPC_ADJ;

    /* Enforce guest required alignment.  */
    if (unlikely(a_bits > 0 && (addr & ((1 << a_bits) - 1)))) {
        /* ??? Maybe indicate atomic op to cpu_unaligned_access */
        cpu_unaligned_access(ENV_GET_CPU(env), addr, MMU_DATA_STORE,
                             mmu_idx, retaddr);
    }

    /* Enforce qemu required alignment.  */
    if (unlikely(addr & ((1 << s_bits) - 1))) {
        /* We get here if guest alignment was not requested,
           or was not enforced by cpu_unaligned_access above.
           We might widen the access and emulate, but for now
           mark an exception and exit the cpu loop.  */
        goto stop_the_world;
    }

    /* Check TLB entry and enforce page permissions.  */
    if ((addr & TARGET_PAGE_MASK)
        != (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {
        if (!VICTIM_TLB_HIT(addr_write, addr)) {
            tlb_fill(ENV_GET_CPU(env), addr, 1 << s_bits, MMU_DATA_STORE,
                     mmu_idx, retaddr);
        }
        tlb_addr = tlbe->addr_write;
    }

    /* Check notdirty */
    if (unlikely(tlb_addr & TLB_NOTDIRTY)) {
        tlb_set_dirty(ENV_GET_CPU(env), addr);
        tlb_addr = tlb_addr & ~TLB_NOTDIRTY;
    }

    /* Notice an IO access  */
    if (unlikely(tlb_addr & ~TARGET_PAGE_MASK)) {
        /* There's really nothing that can be done to
           support this apart from stop-the-world.  */
        goto stop_the_world;
    }

    /* Let the guest notice RMW on a write-only page.  */
    if (unlikely(tlbe->addr_read != tlb_addr)) {
        tlb_fill(ENV_GET_CPU(env), addr, 1 << s_bits, MMU_DATA_LOAD,
                 mmu_idx, retaddr);
        /* Since we don't support reads and writes to different addresses,
           and we do have the proper page loaded for write, this shouldn't
           ever return.  But just in case, handle via stop-the-world.  */
        goto stop_the_world;
    }

    return (void *)((uintptr_t)addr + tlbe->addend);

 stop_the_world:
    cpu_loop_exit_atomic(ENV_GET_CPU(env), retaddr);
}

#ifdef TARGET_WORDS_BIGENDIAN
# define TGT_BE(X)  (X)
# define TGT_LE(X)  BSWAP(X)
#else
# define TGT_BE(X)  BSWAP(X)
# define TGT_LE(X)  (X)
#endif

#define MMUSUFFIX _mmu

#define DATA_SIZE 1
#include "softmmu_template.h"

#define DATA_SIZE 2
#include "softmmu_template.h"

#define DATA_SIZE 4
#include "softmmu_template.h"

#define DATA_SIZE 8
#include "softmmu_template.h"

/* First set of helpers allows passing in of OI and RETADDR.  This makes
   them callable from other helpers.  */

#define EXTRA_ARGS     , TCGMemOpIdx oi, uintptr_t retaddr
#define ATOMIC_NAME(X) \
    HELPER(glue(glue(glue(atomic_ ## X, SUFFIX), END), _mmu))
#define ATOMIC_MMU_LOOKUP  atomic_mmu_lookup(env, addr, oi, retaddr)
#define ATOMIC_MMU_CLEANUP do { } while (0)

#define DATA_SIZE 1
#include "atomic_template.h"

#define DATA_SIZE 2
#include "atomic_template.h"

#define DATA_SIZE 4
#include "atomic_template.h"

#ifdef CONFIG_ATOMIC64
#define DATA_SIZE 8
#include "atomic_template.h"
#endif

#ifdef CONFIG_ATOMIC128
#define DATA_SIZE 16
#include "atomic_template.h"
#endif

/* Second set of helpers are directly callable from TCG as helpers.  */

#undef EXTRA_ARGS
#undef ATOMIC_NAME
#undef ATOMIC_MMU_LOOKUP
#define EXTRA_ARGS         , TCGMemOpIdx oi
#define ATOMIC_NAME(X)     HELPER(glue(glue(atomic_ ## X, SUFFIX), END))
#define ATOMIC_MMU_LOOKUP  atomic_mmu_lookup(env, addr, oi, GETPC())

#define DATA_SIZE 1
#include "atomic_template.h"

#define DATA_SIZE 2
#include "atomic_template.h"

#define DATA_SIZE 4
#include "atomic_template.h"

#ifdef CONFIG_ATOMIC64
#define DATA_SIZE 8
#include "atomic_template.h"
#endif

/* Code access functions.  */

#undef MMUSUFFIX
#define MMUSUFFIX _cmmu
#undef GETPC
#define GETPC() ((uintptr_t)0)
#define SOFTMMU_CODE_ACCESS

#define DATA_SIZE 1
#include "softmmu_template.h"

#define DATA_SIZE 2
#include "softmmu_template.h"

#define DATA_SIZE 4
#include "softmmu_template.h"

#define DATA_SIZE 8
#include "softmmu_template.h"
