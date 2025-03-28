/*
 *  Software MMU support
 *
 * Generate helpers used by TCG for qemu_ld/st ops and code load
 * functions.
 *
 * Included from target op helpers and exec.c.
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

#if DATA_SIZE == 8
#define SUFFIX q
#define LSUFFIX q
#define SDATA_TYPE  int64_t
#define DATA_TYPE  uint64_t
#elif DATA_SIZE == 4
#define SUFFIX l
#define LSUFFIX l
#define SDATA_TYPE  int32_t
#define DATA_TYPE  uint32_t
#elif DATA_SIZE == 2
#define SUFFIX w
#define LSUFFIX uw
#define SDATA_TYPE  int16_t
#define DATA_TYPE  uint16_t
#elif DATA_SIZE == 1
#define SUFFIX b
#define LSUFFIX ub
#define SDATA_TYPE  int8_t
#define DATA_TYPE  uint8_t
#else
#error unsupported data size
#endif


/* For the benefit of TCG generated code, we want to avoid the complication
   of ABI-specific return type promotion and always return a value extended
   to the register size of the host.  This is tcg_target_long, except in the
   case of a 32-bit host and 64-bit data, and for that we always have
   uint64_t.  Don't bother with this widened value for SOFTMMU_CODE_ACCESS.  */
#if defined(SOFTMMU_CODE_ACCESS) || DATA_SIZE == 8
# define WORD_TYPE  DATA_TYPE
# define USUFFIX    SUFFIX
#else
# define WORD_TYPE  tcg_target_ulong
# define USUFFIX    glue(u, SUFFIX)
# define SSUFFIX    glue(s, SUFFIX)
#endif

#ifdef SOFTMMU_CODE_ACCESS
#define READ_ACCESS_TYPE MMU_INST_FETCH
#define ADDR_READ addr_code
#else
#define READ_ACCESS_TYPE MMU_DATA_LOAD
#define ADDR_READ addr_read
#endif

#if DATA_SIZE == 8
# define BSWAP(X)  bswap64(X)
#elif DATA_SIZE == 4
# define BSWAP(X)  bswap32(X)
#elif DATA_SIZE == 2
# define BSWAP(X)  bswap16(X)
#else
# define BSWAP(X)  (X)
#endif

#if DATA_SIZE == 1
# define helper_le_ld_name  glue(glue(helper_ret_ld, USUFFIX), MMUSUFFIX)
# define helper_be_ld_name  helper_le_ld_name
# define helper_le_lds_name glue(glue(helper_ret_ld, SSUFFIX), MMUSUFFIX)
# define helper_be_lds_name helper_le_lds_name
# define helper_le_st_name  glue(glue(helper_ret_st, SUFFIX), MMUSUFFIX)
# define helper_be_st_name  helper_le_st_name
#else
# define helper_le_ld_name  glue(glue(helper_le_ld, USUFFIX), MMUSUFFIX)
# define helper_be_ld_name  glue(glue(helper_be_ld, USUFFIX), MMUSUFFIX)
# define helper_le_lds_name glue(glue(helper_le_ld, SSUFFIX), MMUSUFFIX)
# define helper_be_lds_name glue(glue(helper_be_ld, SSUFFIX), MMUSUFFIX)
# define helper_le_st_name  glue(glue(helper_le_st, SUFFIX), MMUSUFFIX)
# define helper_be_st_name  glue(glue(helper_be_st, SUFFIX), MMUSUFFIX)
#endif

#ifndef SOFTMMU_CODE_ACCESS
static inline DATA_TYPE glue(io_read, SUFFIX)(CPUArchState *env,
                                              size_t mmu_idx, size_t index,
                                              target_ulong addr,
                                              uintptr_t retaddr)
{
    CPUIOTLBEntry *iotlbentry = &env->iotlb[mmu_idx][index];
    return io_readx(env, iotlbentry, mmu_idx, addr, retaddr, DATA_SIZE);
}
#endif

WORD_TYPE helper_le_ld_name(CPUArchState *env, target_ulong addr,
                            TCGMemOpIdx oi, uintptr_t retaddr)
{
    unsigned mmu_idx = get_mmuidx(oi);
    int index = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    target_ulong tlb_addr = env->tlb_table[mmu_idx][index].ADDR_READ;
    unsigned a_bits = get_alignment_bits(get_memop(oi));
    uintptr_t haddr;
    DATA_TYPE res;
    int error_code;
    struct hook *hook;
    bool handled;
    HOOK_FOREACH_VAR_DECLARE;

    struct uc_struct *uc = env->uc;
    MemoryRegion *mr = memory_mapping(uc, addr);

    // memory might be still unmapped while reading or fetching
    if (mr == NULL) {
        handled = false;
#if defined(SOFTMMU_CODE_ACCESS)
        error_code = UC_ERR_FETCH_UNMAPPED;
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_FETCH_UNMAPPED) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            if ((handled = ((uc_cb_eventmem_t)hook->callback)(uc, UC_MEM_FETCH_UNMAPPED, addr, DATA_SIZE, 0, hook->user_data)))
                break;
        }
#else
        error_code = UC_ERR_READ_UNMAPPED;
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_READ_UNMAPPED) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            if ((handled = ((uc_cb_eventmem_t)hook->callback)(uc, UC_MEM_READ_UNMAPPED, addr, DATA_SIZE, 0, hook->user_data)))
                break;
        }
#endif
        if (handled) {
            env->invalid_error = UC_ERR_OK;
            mr = memory_mapping(uc, addr);  // FIXME: what if mr is still NULL at this time?
        } else {
            env->invalid_addr = addr;
            env->invalid_error = error_code;
            // printf("***** Invalid fetch (unmapped memory) at " TARGET_FMT_lx "\n", addr);
            cpu_exit(uc->current_cpu);
            return 0;
        }
    }

#if defined(SOFTMMU_CODE_ACCESS)
    // Unicorn: callback on fetch from NX
    if (mr != NULL && !(mr->perms & UC_PROT_EXEC)) {  // non-executable
        handled = false;
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_FETCH_PROT) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            if ((handled = ((uc_cb_eventmem_t)hook->callback)(uc, UC_MEM_FETCH_PROT, addr, DATA_SIZE, 0, hook->user_data)))
                break;
        }

        if (handled) {
            env->invalid_error = UC_ERR_OK;
        } else {
            env->invalid_addr = addr;
            env->invalid_error = UC_ERR_FETCH_PROT;
            // printf("***** Invalid fetch (non-executable) at " TARGET_FMT_lx "\n", addr);
            cpu_exit(uc->current_cpu);
            return 0;
        }
    }
#endif

    // Unicorn: callback on memory read
    // NOTE: this happens before the actual read, so we cannot tell
    // the callback if read access is succesful, or not.
    // See UC_HOOK_MEM_READ_AFTER & UC_MEM_READ_AFTER if you only care
    // about successful read
    if (READ_ACCESS_TYPE == MMU_DATA_LOAD) {
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_READ) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            ((uc_cb_hookmem_t)hook->callback)(env->uc, UC_MEM_READ, addr, DATA_SIZE, 0, hook->user_data);
        }
    }

    // Unicorn: callback on non-readable memory
    if (READ_ACCESS_TYPE == MMU_DATA_LOAD && mr != NULL && !(mr->perms & UC_PROT_READ)) {  //non-readable
        handled = false;
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_READ_PROT) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            if ((handled = ((uc_cb_eventmem_t)hook->callback)(uc, UC_MEM_READ_PROT, addr, DATA_SIZE, 0, hook->user_data)))
                break;
        }

        if (handled) {
            env->invalid_error = UC_ERR_OK;
        } else {
            env->invalid_addr = addr;
            env->invalid_error = UC_ERR_READ_PROT;
            // printf("***** Invalid memory read (non-readable) at " TARGET_FMT_lx "\n", addr);
            cpu_exit(uc->current_cpu);
            return 0;
        }
    }

    if (addr & ((1 << a_bits) - 1)) {
        cpu_unaligned_access(ENV_GET_CPU(env), addr, READ_ACCESS_TYPE,
                             mmu_idx, retaddr);
    }

    /* If the TLB entry is for a different page, reload and try again.  */
    if ((addr & TARGET_PAGE_MASK)
         != (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {
        if (!VICTIM_TLB_HIT(ADDR_READ, addr)) {
            tlb_fill(ENV_GET_CPU(env), addr, DATA_SIZE, READ_ACCESS_TYPE,
                     mmu_idx, retaddr);
        }
        tlb_addr = env->tlb_table[mmu_idx][index].ADDR_READ;
    }

    /* Handle an IO access.  */
    if (unlikely(tlb_addr & ~TARGET_PAGE_MASK)) {
        CPUIOTLBEntry *iotlbentry;
        if ((addr & (DATA_SIZE - 1)) != 0) {
            goto do_unaligned_access;
        }
        iotlbentry = &env->iotlb[mmu_idx][index];
        if (iotlbentry->addr == 0) {
            env->invalid_addr = addr;
            env->invalid_error = UC_ERR_READ_UNMAPPED;
            // printf("Invalid memory read at " TARGET_FMT_lx "\n", addr);
            cpu_exit(env->uc->current_cpu);
            return 0;
        } else {
            env->invalid_error = UC_ERR_OK;
        }

        /* ??? Note that the io helpers always read data in the target
           byte ordering.  We should push the LE/BE request down into io.  */
        res = glue(io_read, SUFFIX)(env, mmu_idx, index, addr, retaddr);
        res = TGT_LE(res);
        goto _out;
    }

    /* Handle slow unaligned access (it spans two pages or IO).  */
    if (DATA_SIZE > 1
        && unlikely((addr & ~TARGET_PAGE_MASK) + DATA_SIZE - 1
                    >= TARGET_PAGE_SIZE)) {
        target_ulong addr1, addr2;
        DATA_TYPE res1, res2;
        unsigned shift;
    do_unaligned_access:
        addr1 = addr & ~(DATA_SIZE - 1);
        addr2 = addr1 + DATA_SIZE;
        res1 = helper_le_ld_name(env, addr1, oi, retaddr);
        res2 = helper_le_ld_name(env, addr2, oi, retaddr);
        shift = (addr & (DATA_SIZE - 1)) * 8;

        /* Little-endian combine.  */
        res = (res1 >> shift) | (res2 << ((DATA_SIZE * 8) - shift));
        goto _out;
    }

    haddr = (uintptr_t)(addr + env->tlb_table[mmu_idx][index].addend);
#if DATA_SIZE == 1
    res = glue(glue(ld, LSUFFIX), _p)((uint8_t *)haddr);
#else
    res = glue(glue(ld, LSUFFIX), _le_p)((uint8_t *)haddr);
#endif

_out:
    // Unicorn: callback on successful read
    if (READ_ACCESS_TYPE == MMU_DATA_LOAD) {
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_READ_AFTER) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            ((uc_cb_hookmem_t)hook->callback)(env->uc, UC_MEM_READ_AFTER, addr, DATA_SIZE, res, hook->user_data);
        }
    }

    return res;
}

#if DATA_SIZE > 1
WORD_TYPE helper_be_ld_name(CPUArchState *env, target_ulong addr,
                            TCGMemOpIdx oi, uintptr_t retaddr)
{
    unsigned mmu_idx = get_mmuidx(oi);
    int index = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    target_ulong tlb_addr = env->tlb_table[mmu_idx][index].ADDR_READ;
    unsigned a_bits = get_alignment_bits(get_memop(oi));
    uintptr_t haddr;
    DATA_TYPE res;
    int error_code;
    struct hook *hook;
    bool handled;
    HOOK_FOREACH_VAR_DECLARE;

    struct uc_struct *uc = env->uc;
    MemoryRegion *mr = memory_mapping(uc, addr);

    // memory can be unmapped while reading or fetching
    if (mr == NULL) {
        handled = false;
#if defined(SOFTMMU_CODE_ACCESS)
        error_code = UC_ERR_FETCH_UNMAPPED;
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_FETCH_UNMAPPED) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            if ((handled = ((uc_cb_eventmem_t)hook->callback)(uc, UC_MEM_FETCH_UNMAPPED, addr, DATA_SIZE, 0, hook->user_data)))
                break;
        }
#else
        error_code = UC_ERR_READ_UNMAPPED;
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_READ_UNMAPPED) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            if ((handled = ((uc_cb_eventmem_t)hook->callback)(uc, UC_MEM_READ_UNMAPPED, addr, DATA_SIZE, 0, hook->user_data)))
                break;
        }
#endif
        if (handled) {
            env->invalid_error = UC_ERR_OK;
            mr = memory_mapping(uc, addr);  // FIXME: what if mr is still NULL at this time?
        } else {
            env->invalid_addr = addr;
            env->invalid_error = error_code;
            // printf("***** Invalid fetch (unmapped memory) at " TARGET_FMT_lx "\n", addr);
            cpu_exit(uc->current_cpu);
            return 0;
        }
    }

#if defined(SOFTMMU_CODE_ACCESS)
    // Unicorn: callback on fetch from NX
    if (mr != NULL && !(mr->perms & UC_PROT_EXEC)) {  // non-executable
        handled = false;
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_FETCH_PROT) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            if ((handled = ((uc_cb_eventmem_t)hook->callback)(uc, UC_MEM_FETCH_PROT, addr, DATA_SIZE, 0, hook->user_data)))
                break;
        }

        if (handled) {
            env->invalid_error = UC_ERR_OK;
        } else {
            env->invalid_addr = addr;
            env->invalid_error = UC_ERR_FETCH_PROT;
            // printf("***** Invalid fetch (non-executable) at " TARGET_FMT_lx "\n", addr);
            cpu_exit(uc->current_cpu);
            return 0;
        }
    }
#endif

    // Unicorn: callback on memory read
    // NOTE: this happens before the actual read, so we cannot tell
    // the callback if read access is succesful, or not.
    // See UC_HOOK_MEM_READ_AFTER & UC_MEM_READ_AFTER if you only care
    // about successful read
    if (READ_ACCESS_TYPE == MMU_DATA_LOAD) {
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_READ) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            ((uc_cb_hookmem_t)hook->callback)(env->uc, UC_MEM_READ, addr, DATA_SIZE, 0, hook->user_data);
        }
    }

    // Unicorn: callback on non-readable memory
    if (READ_ACCESS_TYPE == MMU_DATA_LOAD && mr != NULL && !(mr->perms & UC_PROT_READ)) {  //non-readable
        handled = false;
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_READ_PROT) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            if ((handled = ((uc_cb_eventmem_t)hook->callback)(uc, UC_MEM_READ_PROT, addr, DATA_SIZE, 0, hook->user_data)))
                break;
        }

        if (handled) {
            env->invalid_error = UC_ERR_OK;
        } else {
            env->invalid_addr = addr;
            env->invalid_error = UC_ERR_READ_PROT;
            // printf("***** Invalid memory read (non-readable) at " TARGET_FMT_lx "\n", addr);
            cpu_exit(uc->current_cpu);
            return 0;
        }
    }

    if (addr & ((1 << a_bits) - 1)) {
        cpu_unaligned_access(ENV_GET_CPU(env), addr, READ_ACCESS_TYPE,
                             mmu_idx, retaddr);
    }

    /* If the TLB entry is for a different page, reload and try again.  */
    if ((addr & TARGET_PAGE_MASK)
         != (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {
        if (!VICTIM_TLB_HIT(ADDR_READ, addr)) {
            tlb_fill(ENV_GET_CPU(env), addr, DATA_SIZE, READ_ACCESS_TYPE,
                     mmu_idx, retaddr);
        }
        tlb_addr = env->tlb_table[mmu_idx][index].ADDR_READ;
    }

    /* Handle an IO access.  */
    if (unlikely(tlb_addr & ~TARGET_PAGE_MASK)) {
        CPUIOTLBEntry *iotlbentry;
        if ((addr & (DATA_SIZE - 1)) != 0) {
            goto do_unaligned_access;
        }
        iotlbentry = &env->iotlb[mmu_idx][index];

        if (iotlbentry->addr == 0) {
            env->invalid_addr = addr;
            env->invalid_error = UC_ERR_READ_UNMAPPED;
            // printf("Invalid memory read at " TARGET_FMT_lx "\n", addr);
            cpu_exit(env->uc->current_cpu);
            return 0;
        }

        /* ??? Note that the io helpers always read data in the target
           byte ordering.  We should push the LE/BE request down into io.  */
        res = glue(io_read, SUFFIX)(env, mmu_idx, index, addr, retaddr);
        res = TGT_BE(res);
        goto _out;
    }

    /* Handle slow unaligned access (it spans two pages or IO).  */
    if (DATA_SIZE > 1
        && unlikely((addr & ~TARGET_PAGE_MASK) + DATA_SIZE - 1
                    >= TARGET_PAGE_SIZE)) {
        target_ulong addr1, addr2;
        DATA_TYPE res1, res2;
        unsigned shift;
    do_unaligned_access:
        addr1 = addr & ~(DATA_SIZE - 1);
        addr2 = addr1 + DATA_SIZE;
        res1 = helper_be_ld_name(env, addr1, oi, retaddr);
        res2 = helper_be_ld_name(env, addr2, oi, retaddr);
        shift = (addr & (DATA_SIZE - 1)) * 8;

        /* Big-endian combine.  */
        res = (res1 << shift) | (res2 >> ((DATA_SIZE * 8) - shift));
        goto _out;
    }

    haddr = (uintptr_t)(addr + env->tlb_table[mmu_idx][index].addend);
    res = glue(glue(ld, LSUFFIX), _be_p)((uint8_t *)haddr);

_out:
    // Unicorn: callback on successful read
    if (READ_ACCESS_TYPE == MMU_DATA_LOAD) {
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_READ_AFTER) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            ((uc_cb_hookmem_t)hook->callback)(env->uc, UC_MEM_READ_AFTER, addr, DATA_SIZE, res, hook->user_data);
        }
    }

    return res;
}
#endif /* DATA_SIZE > 1 */

#ifndef SOFTMMU_CODE_ACCESS

/* Provide signed versions of the load routines as well.  We can of course
   avoid this for 64-bit data, or for 32-bit data on 32-bit host.  */
#if DATA_SIZE * 8 < TCG_TARGET_REG_BITS
WORD_TYPE helper_le_lds_name(CPUArchState *env, target_ulong addr,
                             TCGMemOpIdx oi, uintptr_t retaddr)
{
    return (SDATA_TYPE)helper_le_ld_name(env, addr, oi, retaddr);
}

# if DATA_SIZE > 1
WORD_TYPE helper_be_lds_name(CPUArchState *env, target_ulong addr,
                             TCGMemOpIdx oi, uintptr_t retaddr)
{
    return (SDATA_TYPE)helper_be_ld_name(env, addr, oi, retaddr);
}
# endif
#endif

static inline void glue(io_write, SUFFIX)(CPUArchState *env,
                                          size_t mmu_idx, size_t index,
                                          DATA_TYPE val,
                                          target_ulong addr,
                                          uintptr_t retaddr)
{
    CPUIOTLBEntry *iotlbentry = &env->iotlb[mmu_idx][index];
    return io_writex(env, iotlbentry, mmu_idx, val, addr, retaddr, DATA_SIZE);
}

void helper_le_st_name(CPUArchState *env, target_ulong addr, DATA_TYPE val,
                       TCGMemOpIdx oi, uintptr_t retaddr)
{
    unsigned mmu_idx = get_mmuidx(oi);
    int index = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    target_ulong tlb_addr = env->tlb_table[mmu_idx][index].addr_write;
    unsigned a_bits = get_alignment_bits(get_memop(oi));
    uintptr_t haddr;
    struct hook *hook;
    bool handled;
    HOOK_FOREACH_VAR_DECLARE;

    struct uc_struct *uc = env->uc;
    MemoryRegion *mr = memory_mapping(uc, addr);

    // Unicorn: callback on memory write
    HOOK_FOREACH(uc, hook, UC_HOOK_MEM_WRITE) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
        ((uc_cb_hookmem_t)hook->callback)(uc, UC_MEM_WRITE, addr, DATA_SIZE, val, hook->user_data);
    }

    // Unicorn: callback on invalid memory
    if (mr == NULL) {
        handled = false;
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_WRITE_UNMAPPED) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            if ((handled = ((uc_cb_eventmem_t)hook->callback)(uc, UC_MEM_WRITE_UNMAPPED, addr, DATA_SIZE, val, hook->user_data)))
                break;
        }

        if (!handled) {
            // save error & quit
            env->invalid_addr = addr;
            env->invalid_error = UC_ERR_WRITE_UNMAPPED;
            // printf("***** Invalid memory write at " TARGET_FMT_lx "\n", addr);
            cpu_exit(uc->current_cpu);
            return;
        } else {
            env->invalid_error = UC_ERR_OK;
            mr = memory_mapping(uc, addr);  // FIXME: what if mr is still NULL at this time?
        }
    }

    // Unicorn: callback on non-writable memory
    if (mr != NULL && !(mr->perms & UC_PROT_WRITE)) {  //non-writable
        handled = false;
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_WRITE_PROT) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            if ((handled = ((uc_cb_eventmem_t)hook->callback)(uc, UC_MEM_WRITE_PROT, addr, DATA_SIZE, val, hook->user_data)))
                break;
        }

        if (handled) {
            env->invalid_error = UC_ERR_OK;
        } else {
            env->invalid_addr = addr;
            env->invalid_error = UC_ERR_WRITE_PROT;
            // printf("***** Invalid memory write (ro) at " TARGET_FMT_lx "\n", addr);
            cpu_exit(uc->current_cpu);
            return;
        }
    }

    if (addr & ((1 << a_bits) - 1)) {
        cpu_unaligned_access(ENV_GET_CPU(env), addr, MMU_DATA_STORE,
                             mmu_idx, retaddr);
    }

    /* If the TLB entry is for a different page, reload and try again.  */
    if ((addr & TARGET_PAGE_MASK)
        != (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {
        if (!VICTIM_TLB_HIT(addr_write, addr)) {
            tlb_fill(ENV_GET_CPU(env), addr, DATA_SIZE, MMU_DATA_STORE,
                     mmu_idx, retaddr);
        }
        tlb_addr = env->tlb_table[mmu_idx][index].addr_write;
    }

    /* Handle an IO access.  */
    if (unlikely(tlb_addr & ~TARGET_PAGE_MASK)) {
        CPUIOTLBEntry *iotlbentry;
        if ((addr & (DATA_SIZE - 1)) != 0) {
            goto do_unaligned_access;
        }
        iotlbentry = &env->iotlb[mmu_idx][index];
        if (iotlbentry->addr == 0) {
            env->invalid_addr = addr;
            env->invalid_error = UC_ERR_WRITE_UNMAPPED;
            // printf("***** Invalid memory write at " TARGET_FMT_lx "\n", addr);
            cpu_exit(env->uc->current_cpu);
            return;
        }

        /* ??? Note that the io helpers always read data in the target
           byte ordering.  We should push the LE/BE request down into io.  */
        val = TGT_LE(val);
        glue(io_write, SUFFIX)(env, mmu_idx, index, val, addr, retaddr);
        return;
    }

    /* Handle slow unaligned access (it spans two pages or IO).  */
    if (DATA_SIZE > 1
        && unlikely((addr & ~TARGET_PAGE_MASK) + DATA_SIZE - 1
                     >= TARGET_PAGE_SIZE)) {
        int i, index2;
        target_ulong page2, tlb_addr2;
    do_unaligned_access:
        /* Ensure the second page is in the TLB.  Note that the first page
           is already guaranteed to be filled, and that the second page
           cannot evict the first.  */
        page2 = (addr + DATA_SIZE) & TARGET_PAGE_MASK;
        index2 = (page2 >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
        tlb_addr2 = env->tlb_table[mmu_idx][index2].addr_write;
        if (page2 != (tlb_addr2 & (TARGET_PAGE_MASK | TLB_INVALID_MASK))
            && !VICTIM_TLB_HIT(addr_write, page2)) {
            tlb_fill(ENV_GET_CPU(env), page2, DATA_SIZE, MMU_DATA_STORE,
                     mmu_idx, retaddr);
        }

        /* XXX: not efficient, but simple.  */
        /* This loop must go in the forward direction to avoid issues
           with self-modifying code in Windows 64-bit.  */
        for (i = 0; i < DATA_SIZE; ++i) {
            /* Little-endian extract.  */
            uint8_t val8 = (uint8_t)(val >> (i * 8));
            glue(helper_ret_stb, MMUSUFFIX)(env, addr + i, val8,
                                            oi, retaddr);
            if (env->invalid_error != UC_ERR_OK)
                break;
        }
        return;
    }

    haddr = (uintptr_t)(addr + env->tlb_table[mmu_idx][index].addend);
#if DATA_SIZE == 1
    glue(glue(st, SUFFIX), _p)((uint8_t *)haddr, val);
#else
    glue(glue(st, SUFFIX), _le_p)((uint8_t *)haddr, val);
#endif
}

#if DATA_SIZE > 1
void helper_be_st_name(CPUArchState *env, target_ulong addr, DATA_TYPE val,
                       TCGMemOpIdx oi, uintptr_t retaddr)
{
    unsigned mmu_idx = get_mmuidx(oi);
    int index = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
    target_ulong tlb_addr = env->tlb_table[mmu_idx][index].addr_write;
    unsigned a_bits = get_alignment_bits(get_memop(oi));
    uintptr_t haddr;
    struct hook *hook;
    bool handled;
    HOOK_FOREACH_VAR_DECLARE;

    struct uc_struct *uc = env->uc;
    MemoryRegion *mr = memory_mapping(uc, addr);

    // Unicorn: callback on memory write
    HOOK_FOREACH(uc, hook, UC_HOOK_MEM_WRITE) {
        if (!HOOK_BOUND_CHECK(hook, addr))
            continue;
        ((uc_cb_hookmem_t)hook->callback)(uc, UC_MEM_WRITE, addr, DATA_SIZE, val, hook->user_data);
    }

    // Unicorn: callback on invalid memory
    if (mr == NULL) {
        handled = false;
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_WRITE_UNMAPPED) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            if ((handled = ((uc_cb_eventmem_t)hook->callback)(uc, UC_MEM_WRITE_UNMAPPED, addr, DATA_SIZE, val, hook->user_data)))
                break;
        }

        if (!handled) {
            // save error & quit
            env->invalid_addr = addr;
            env->invalid_error = UC_ERR_WRITE_UNMAPPED;
            // printf("***** Invalid memory write at " TARGET_FMT_lx "\n", addr);
            cpu_exit(uc->current_cpu);
            return;
        } else {
            env->invalid_error = UC_ERR_OK;
            mr = memory_mapping(uc, addr);  // FIXME: what if mr is still NULL at this time?
        }
    }

    // Unicorn: callback on non-writable memory
    if (mr != NULL && !(mr->perms & UC_PROT_WRITE)) {  //non-writable
        handled = false;
        HOOK_FOREACH(uc, hook, UC_HOOK_MEM_WRITE_PROT) {
            if (!HOOK_BOUND_CHECK(hook, addr))
                continue;
            if ((handled = ((uc_cb_eventmem_t)hook->callback)(uc, UC_MEM_WRITE_PROT, addr, DATA_SIZE, val, hook->user_data)))
                break;
        }

        if (handled) {
            env->invalid_error = UC_ERR_OK;
        } else {
            env->invalid_addr = addr;
            env->invalid_error = UC_ERR_WRITE_PROT;
            // printf("***** Invalid memory write (ro) at " TARGET_FMT_lx "\n", addr);
            cpu_exit(uc->current_cpu);
            return;
        }
    }

    if (addr & ((1 << a_bits) - 1)) {
        cpu_unaligned_access(ENV_GET_CPU(env), addr, MMU_DATA_STORE,
                             mmu_idx, retaddr);
    }

    /* If the TLB entry is for a different page, reload and try again.  */
    if ((addr & TARGET_PAGE_MASK)
        != (tlb_addr & (TARGET_PAGE_MASK | TLB_INVALID_MASK))) {
        if (!VICTIM_TLB_HIT(addr_write, addr)) {
            tlb_fill(ENV_GET_CPU(env), addr, DATA_SIZE, MMU_DATA_STORE,
                     mmu_idx, retaddr);
        }
        tlb_addr = env->tlb_table[mmu_idx][index].addr_write;
    }

    /* Handle an IO access.  */
    if (unlikely(tlb_addr & ~TARGET_PAGE_MASK)) {
        CPUIOTLBEntry *iotlbentry;
        if ((addr & (DATA_SIZE - 1)) != 0) {
            goto do_unaligned_access;
        }
        iotlbentry = &env->iotlb[mmu_idx][index];
        if (iotlbentry->addr == 0) {
            env->invalid_addr = addr;
            env->invalid_error = UC_ERR_WRITE_UNMAPPED;
            // printf("***** Invalid memory write at " TARGET_FMT_lx "\n", addr);
            cpu_exit(env->uc->current_cpu);
            return;
        }

        /* ??? Note that the io helpers always read data in the target
           byte ordering.  We should push the LE/BE request down into io.  */
        val = TGT_BE(val);
        glue(io_write, SUFFIX)(env, mmu_idx, index, val, addr, retaddr);
        return;
    }

    /* Handle slow unaligned access (it spans two pages or IO).  */
    if (DATA_SIZE > 1
        && unlikely((addr & ~TARGET_PAGE_MASK) + DATA_SIZE - 1
                     >= TARGET_PAGE_SIZE)) {
        int i, index2;
        target_ulong page2, tlb_addr2;
    do_unaligned_access:
        /* Ensure the second page is in the TLB.  Note that the first page
           is already guaranteed to be filled, and that the second page
           cannot evict the first.  */
        page2 = (addr + DATA_SIZE) & TARGET_PAGE_MASK;
        index2 = (page2 >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
        tlb_addr2 = env->tlb_table[mmu_idx][index2].addr_write;
        if (page2 != (tlb_addr2 & (TARGET_PAGE_MASK | TLB_INVALID_MASK))
            && !VICTIM_TLB_HIT(addr_write, page2)) {
            tlb_fill(ENV_GET_CPU(env), addr, DATA_SIZE, MMU_DATA_STORE,
                     mmu_idx, retaddr);
        }

        /* XXX: not efficient, but simple */
        /* This loop must go in the forward direction to avoid issues
           with self-modifying code.  */
        for (i = 0; i < DATA_SIZE; ++i) {
            /* Big-endian extract.  */
            uint8_t val8 = (uint8_t)(val >> (((DATA_SIZE - 1) * 8) - (i * 8)));
            glue(helper_ret_stb, MMUSUFFIX)(env, addr + i, val8,
                                            oi, retaddr);
            if (env->invalid_error != UC_ERR_OK)
                break;
        }
        return;
    }

    haddr = (uintptr_t)(addr + env->tlb_table[mmu_idx][index].addend);
    glue(glue(st, SUFFIX), _be_p)((uint8_t *)haddr, val);
}
#endif /* DATA_SIZE > 1 */
#endif /* !defined(SOFTMMU_CODE_ACCESS) */

#undef READ_ACCESS_TYPE
#undef DATA_TYPE
#undef SUFFIX
#undef LSUFFIX
#undef DATA_SIZE
#undef ADDR_READ
#undef WORD_TYPE
#undef SDATA_TYPE
#undef USUFFIX
#undef SSUFFIX
#undef BSWAP
#undef helper_le_ld_name
#undef helper_be_ld_name
#undef helper_le_lds_name
#undef helper_be_lds_name
#undef helper_le_st_name
#undef helper_be_st_name
