#include "qemu/osdep.h"
#include "cpu.h"
#include "internals.h"
#include "exec/helper-proto.h"
#include "qemu/host-utils.h"
#include "sysemu/sysemu.h"
#include "qemu/bitops.h"
#include "qemu/crc32c.h"
#include "exec/exec-all.h"
#include "exec/cpu_ldst.h"
#include "arm_ldst.h"
#include "fpu/softfloat.h"

#ifndef CONFIG_USER_ONLY
/* Cacheability and shareability attributes for a memory access */
typedef struct ARMCacheAttrs {
    unsigned int attrs:8; /* as in the MAIR register encoding */
    unsigned int shareability:2; /* as in the SH field of the VMSAv8-64 PTEs */
} ARMCacheAttrs;

static bool get_phys_addr(CPUARMState *env, target_ulong address,
                          MMUAccessType access_type, ARMMMUIdx mmu_idx,
                          hwaddr *phys_ptr, MemTxAttrs *attrs, int *prot,
                          target_ulong *page_size,
                          ARMMMUFaultInfo *fi, ARMCacheAttrs *cacheattrs);

static bool get_phys_addr_lpae(CPUARMState *env, target_ulong address,
                               MMUAccessType access_type, ARMMMUIdx mmu_idx,
                               hwaddr *phys_ptr, MemTxAttrs *txattrs, int *prot,
                               target_ulong *page_size_ptr,
                               ARMMMUFaultInfo *fi, ARMCacheAttrs *cacheattrs);

/* Security attributes for an address, as returned by v8m_security_lookup. */
typedef struct V8M_SAttributes {
    bool ns;
    bool nsc;
    uint8_t sregion;
    bool srvalid;
    uint8_t iregion;
    bool irvalid;
} V8M_SAttributes;

static void v8m_security_lookup(CPUARMState *env, uint32_t address,
                                MMUAccessType access_type, ARMMMUIdx mmu_idx,
                                V8M_SAttributes *sattrs);

/* Definitions for the PMCCNTR and PMCR registers */
#define PMCRD   0x8
#define PMCRC   0x4
#define PMCRE   0x1
#endif

static uint64_t raw_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    assert(ri->fieldoffset);
    if (cpreg_field_is_64bit(ri)) {
        return CPREG_FIELD64(env, ri);
    } else {
        return CPREG_FIELD32(env, ri);
    }
}

static void raw_write(CPUARMState *env, const ARMCPRegInfo *ri,
                      uint64_t value)
{
    assert(ri->fieldoffset);
    if (cpreg_field_is_64bit(ri)) {
        CPREG_FIELD64(env, ri) = value;
    } else {
        CPREG_FIELD32(env, ri) = value;
    }
}

static void *raw_ptr(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return (char *)env + ri->fieldoffset;
}

uint64_t read_raw_cp_reg(CPUARMState *env, const ARMCPRegInfo *ri)
{
    /* Raw read of a coprocessor register (as needed for migration, etc). */
    if (ri->type & ARM_CP_CONST) {
        return ri->resetvalue;
    } else if (ri->raw_readfn) {
        return ri->raw_readfn(env, ri);
    } else if (ri->readfn) {
        return ri->readfn(env, ri);
    } else {
        return raw_read(env, ri);
    }
}

static void write_raw_cp_reg(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t v)
{
    /* Raw write of a coprocessor register (as needed for migration, etc).
     * Note that constant registers are treated as write-ignored; the
     * caller should check for success by whether a readback gives the
     * value written.
     */
    if (ri->type & ARM_CP_CONST) {
        return;
    } else if (ri->raw_writefn) {
        ri->raw_writefn(env, ri, v);
    } else if (ri->writefn) {
        ri->writefn(env, ri, v);
    } else {
        raw_write(env, ri, v);
    }
}

static bool raw_accessors_invalid(const ARMCPRegInfo *ri)
{
   /* Return true if the regdef would cause an assertion if you called
    * read_raw_cp_reg() or write_raw_cp_reg() on it (ie if it is a
    * program bug for it not to have the NO_RAW flag).
    * NB that returning false here doesn't necessarily mean that calling
    * read/write_raw_cp_reg() is safe, because we can't distinguish "has
    * read/write access functions which are safe for raw use" from "has
    * read/write access functions which have side effects but has forgotten
    * to provide raw access functions".
    * The tests here line up with the conditions in read/write_raw_cp_reg()
    * and assertions in raw_read()/raw_write().
    */
    if ((ri->type & ARM_CP_CONST) ||
        ri->fieldoffset ||
        ((ri->raw_writefn || ri->writefn) && (ri->raw_readfn || ri->readfn))) {
        return false;
    }
    return true;
}

bool write_cpustate_to_list(ARMCPU *cpu)
{
    /* Write the coprocessor state from cpu->env to the (index,value) list. */
    int i;
    bool ok = true;

    for (i = 0; i < cpu->cpreg_array_len; i++) {
        uint32_t regidx = kvm_to_cpreg_id(cpu->cpreg_indexes[i]);
        const ARMCPRegInfo *ri;

        ri = get_arm_cp_reginfo(cpu->cp_regs, regidx);
        if (!ri) {
            ok = false;
            continue;
        }
        if (ri->type & ARM_CP_NO_RAW) {
            continue;
        }
        cpu->cpreg_values[i] = read_raw_cp_reg(&cpu->env, ri);
    }
    return ok;
}

bool write_list_to_cpustate(ARMCPU *cpu)
{
    int i;
    bool ok = true;

    for (i = 0; i < cpu->cpreg_array_len; i++) {
        uint32_t regidx = kvm_to_cpreg_id(cpu->cpreg_indexes[i]);
        uint64_t v = cpu->cpreg_values[i];
        const ARMCPRegInfo *ri;

        ri = get_arm_cp_reginfo(cpu->cp_regs, regidx);
        if (!ri) {
            ok = false;
            continue;
        }
        if (ri->type & ARM_CP_NO_RAW) {
            continue;
        }
        /* Write value and confirm it reads back as written
         * (to catch read-only registers and partially read-only
         * registers where the incoming migration value doesn't match)
         */
        write_raw_cp_reg(&cpu->env, ri, v);
        if (read_raw_cp_reg(&cpu->env, ri) != v) {
            ok = false;
        }
    }
    return ok;
}

static void add_cpreg_to_list(gpointer key, gpointer opaque)
{
    ARMCPU *cpu = opaque;
    uint64_t regidx;
    const ARMCPRegInfo *ri;

    regidx = *(uint32_t *)key;
    ri = get_arm_cp_reginfo(cpu->cp_regs, regidx);

    if (!(ri->type & (ARM_CP_NO_RAW|ARM_CP_ALIAS))) {
        cpu->cpreg_indexes[cpu->cpreg_array_len] = cpreg_to_kvm_id(regidx);
        /* The value array need not be initialized at this point */
        cpu->cpreg_array_len++;
    }
}

static void count_cpreg(gpointer key, gpointer opaque)
{
    ARMCPU *cpu = opaque;
    uint64_t regidx;
    const ARMCPRegInfo *ri;

    regidx = *(uint32_t *)key;
    ri = get_arm_cp_reginfo(cpu->cp_regs, regidx);

    if (!(ri->type & (ARM_CP_NO_RAW|ARM_CP_ALIAS))) {
        cpu->cpreg_array_len++;
    }
}

static gint cpreg_key_compare(gconstpointer a, gconstpointer b)
{
    uint64_t aidx = cpreg_to_kvm_id(*(uint32_t *)a);
    uint64_t bidx = cpreg_to_kvm_id(*(uint32_t *)b);

    if (aidx > bidx) {
        return 1;
    }
    if (aidx < bidx) {
        return -1;
    }
    return 0;
}

static void cpreg_make_keylist(gpointer key, gpointer value, gpointer udata)
{
    GList **plist = udata;

    *plist = g_list_prepend(*plist, key);
}

void init_cpreg_list(ARMCPU *cpu)
{
    /* Initialise the cpreg_tuples[] array based on the cp_regs hash.
     * Note that we require cpreg_tuples[] to be sorted by key ID.
     */
    GList *keys = NULL;
    int arraylen;

    g_hash_table_foreach(cpu->cp_regs, cpreg_make_keylist, &keys);

    keys = g_list_sort(keys, cpreg_key_compare);

    cpu->cpreg_array_len = 0;

    g_list_foreach(keys, count_cpreg, cpu);

    arraylen = cpu->cpreg_array_len;
    cpu->cpreg_indexes = g_new(uint64_t, arraylen);
    cpu->cpreg_values = g_new(uint64_t, arraylen);
    cpu->cpreg_vmstate_indexes = g_new(uint64_t, arraylen);
    cpu->cpreg_vmstate_values = g_new(uint64_t, arraylen);
    cpu->cpreg_vmstate_array_len = cpu->cpreg_array_len;
    cpu->cpreg_array_len = 0;

    g_list_foreach(keys, add_cpreg_to_list, cpu);

    assert(cpu->cpreg_array_len == arraylen);

    g_list_free(keys);
}

/*
 * Some registers are not accessible if EL3.NS=0 and EL3 is using AArch32 but
 * they are accessible when EL3 is using AArch64 regardless of EL3.NS.
 *
 * access_el3_aa32ns: Used to check AArch32 register views.
 * access_el3_aa32ns_aa64any: Used to check both AArch32/64 register views.
 */
static CPAccessResult access_el3_aa32ns(CPUARMState *env,
                                        const ARMCPRegInfo *ri,
                                        bool isread)
{
    bool secure = arm_is_secure_below_el3(env);

    assert(!arm_el_is_aa64(env, 3));
    if (secure) {
        return CP_ACCESS_TRAP_UNCATEGORIZED;
    }
    return CP_ACCESS_OK;
}

static CPAccessResult access_el3_aa32ns_aa64any(CPUARMState *env,
                                                const ARMCPRegInfo *ri,
                                                bool isread)
{
    if (!arm_el_is_aa64(env, 3)) {
        return access_el3_aa32ns(env, ri, isread);
    }
    return CP_ACCESS_OK;
}

/* Some secure-only AArch32 registers trap to EL3 if used from
 * Secure EL1 (but are just ordinary UNDEF in other non-EL3 contexts).
 * Note that an access from Secure EL1 can only happen if EL3 is AArch64.
 * We assume that the .access field is set to PL1_RW.
 */
static CPAccessResult access_trap_aa32s_el1(CPUARMState *env,
                                            const ARMCPRegInfo *ri,
                                            bool isread)
{
    if (arm_current_el(env) == 3) {
        return CP_ACCESS_OK;
    }
    if (arm_is_secure_below_el3(env)) {
        return CP_ACCESS_TRAP_EL3;
    }
    /* This will be EL1 NS and EL2 NS, which just UNDEF */
    return CP_ACCESS_TRAP_UNCATEGORIZED;
}

/* Check for traps to "powerdown debug" registers, which are controlled
 * by MDCR.TDOSA
 */
static CPAccessResult access_tdosa(CPUARMState *env, const ARMCPRegInfo *ri,
                                   bool isread)
{
    int el = arm_current_el(env);

    if (el < 2 && (env->cp15.mdcr_el2 & MDCR_TDOSA)
        && !arm_is_secure_below_el3(env)) {
        return CP_ACCESS_TRAP_EL2;
    }
    if (el < 3 && (env->cp15.mdcr_el3 & MDCR_TDOSA)) {
        return CP_ACCESS_TRAP_EL3;
    }
    return CP_ACCESS_OK;
}

/* Check for traps to "debug ROM" registers, which are controlled
 * by MDCR_EL2.TDRA for EL2 but by the more general MDCR_EL3.TDA for EL3.
 */
static CPAccessResult access_tdra(CPUARMState *env, const ARMCPRegInfo *ri,
                                  bool isread)
{
    int el = arm_current_el(env);

    if (el < 2 && (env->cp15.mdcr_el2 & MDCR_TDRA)
        && !arm_is_secure_below_el3(env)) {
        return CP_ACCESS_TRAP_EL2;
    }
    if (el < 3 && (env->cp15.mdcr_el3 & MDCR_TDA)) {
        return CP_ACCESS_TRAP_EL3;
    }
    return CP_ACCESS_OK;
}

/* Check for traps to general debug registers, which are controlled
 * by MDCR_EL2.TDA for EL2 and MDCR_EL3.TDA for EL3.
 */
static CPAccessResult access_tda(CPUARMState *env, const ARMCPRegInfo *ri,
                                  bool isread)
{
    int el = arm_current_el(env);

    if (el < 2 && (env->cp15.mdcr_el2 & MDCR_TDA)
        && !arm_is_secure_below_el3(env)) {
        return CP_ACCESS_TRAP_EL2;
    }
    if (el < 3 && (env->cp15.mdcr_el3 & MDCR_TDA)) {
        return CP_ACCESS_TRAP_EL3;
    }
    return CP_ACCESS_OK;
}

/* Check for traps to performance monitor registers, which are controlled
 * by MDCR_EL2.TPM for EL2 and MDCR_EL3.TPM for EL3.
 */
static CPAccessResult access_tpm(CPUARMState *env, const ARMCPRegInfo *ri,
                                 bool isread)
{
    int el = arm_current_el(env);

    if (el < 2 && (env->cp15.mdcr_el2 & MDCR_TPM)
        && !arm_is_secure_below_el3(env)) {
        return CP_ACCESS_TRAP_EL2;
    }
    if (el < 3 && (env->cp15.mdcr_el3 & MDCR_TPM)) {
        return CP_ACCESS_TRAP_EL3;
    }
    return CP_ACCESS_OK;
}

static void dacr_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    raw_write(env, ri, value);
    tlb_flush(CPU(cpu)); /* Flush TLB as domain not tracked in TLB */
}

static void fcse_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    if (raw_read(env, ri) != value) {
        /* Unlike real hardware the qemu TLB uses virtual addresses,
         * not modified virtual addresses, so this causes a TLB flush.
         */
        tlb_flush(CPU(cpu));
        raw_write(env, ri, value);
    }
}

static void contextidr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    if (raw_read(env, ri) != value && !arm_feature(env, ARM_FEATURE_PMSA)
        && !extended_addresses_enabled(env)) {
        /* For VMSA (when not using the LPAE long descriptor page table
         * format) this register includes the ASID, so do a TLB flush.
         * For PMSA it is purely a process ID and no action is needed.
         */
        tlb_flush(CPU(cpu));
    }
    raw_write(env, ri, value);
}

static void tlbiall_write(CPUARMState *env, const ARMCPRegInfo *ri,
                          uint64_t value)
{
    /* Invalidate all (TLBIALL) */
    ARMCPU *cpu = arm_env_get_cpu(env);

    tlb_flush(CPU(cpu));
}

static void tlbimva_write(CPUARMState *env, const ARMCPRegInfo *ri,
                          uint64_t value)
{
    /* Invalidate single TLB entry by MVA and ASID (TLBIMVA) */
    ARMCPU *cpu = arm_env_get_cpu(env);

    tlb_flush_page(CPU(cpu), value & TARGET_PAGE_MASK);
}

static void tlbiasid_write(CPUARMState *env, const ARMCPRegInfo *ri,
                           uint64_t value)
{
    /* Invalidate by ASID (TLBIASID) */
    ARMCPU *cpu = arm_env_get_cpu(env);

    tlb_flush(CPU(cpu));
}

static void tlbimvaa_write(CPUARMState *env, const ARMCPRegInfo *ri,
                           uint64_t value)
{
    /* Invalidate single entry by MVA, all ASIDs (TLBIMVAA) */
    ARMCPU *cpu = arm_env_get_cpu(env);

    tlb_flush_page(CPU(cpu), value & TARGET_PAGE_MASK);
}

/* IS variants of TLB operations must affect all cores */
static void tlbiall_is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t value)
{
    //struct uc_struct *uc = env->uc;
    // TODO: issue #642
    // tlb_flush(other_cpu);
}

static void tlbiasid_is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t value)
{
    //struct uc_struct *uc = env->uc;
    // TODO: issue #642
    // tlb_flush(other_cpu);
}

static void tlbimva_is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t value)
{
    //struct uc_struct *uc = env->uc;
    // TODO: issue #642
    // tlb_flush(other_cpu);
}

static void tlbimvaa_is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t value)
{
    //struct uc_struct *uc = env->uc;
    // TODO: issue #642
    // tlb_flush(other_cpu);
}

static void tlbiall_nsnh_write(CPUARMState *env, const ARMCPRegInfo *ri,
                               uint64_t value)
{
    CPUState *cs = ENV_GET_CPU(env);

    tlb_flush_by_mmuidx(cs,
                        ARMMMUIdxBit_S12NSE1 |
                        ARMMMUIdxBit_S12NSE0 |
                        ARMMMUIdxBit_S2NS);
}

static void tlbiall_nsnh_is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                  uint64_t value)
{
  // Unicorn: if'd out. See issue 642
#if 0
    CPUState *cs = ENV_GET_CPU(env);

    tlb_flush_by_mmuidx_all_cpus_synced(cs,
                                        ARMMMUIdxBit_S12NSE1 |
                                        ARMMMUIdxBit_S12NSE0 |
                                        ARMMMUIdxBit_S2NS);
#endif
}

static void tlbiipas2_write(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    /* Invalidate by IPA. This has to invalidate any structures that
     * contain only stage 2 translation information, but does not need
     * to apply to structures that contain combined stage 1 and stage 2
     * translation information.
     * This must NOP if EL2 isn't implemented or SCR_EL3.NS is zero.
     */
    CPUState *cs = ENV_GET_CPU(env);
    uint64_t pageaddr;

    if (!arm_feature(env, ARM_FEATURE_EL2) || !(env->cp15.scr_el3 & SCR_NS)) {
        return;
    }

    pageaddr = sextract64(value << 12, 0, 40);

    tlb_flush_page_by_mmuidx(cs, pageaddr, ARMMMUIdxBit_S2NS);
}

static void tlbiipas2_is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                               uint64_t value)
{
  // Unicorn: if'd out, see issue 642
#if 0
    CPUState *other_cs;
    uint64_t pageaddr;

    if (!arm_feature(env, ARM_FEATURE_EL2) || !(env->cp15.scr_el3 & SCR_NS)) {
        return;
    }

    pageaddr = sextract64(value << 12, 0, 40);

    tlb_flush_page_by_mmuidx_all_cpus_synced(cs, pageaddr,
                                             ARMMMUIdxBit_S2NS);
#endif
}

static void tlbiall_hyp_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    CPUState *cs = ENV_GET_CPU(env);

    tlb_flush_by_mmuidx(cs, ARMMMUIdxBit_S1E2);
}

static void tlbiall_hyp_is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                 uint64_t value)
{
    // Unicorn: if'd out. See issue 642
#if 0
    CPUState *cs = ENV_GET_CPU(env);

    tlb_flush_by_mmuidx_all_cpus_synced(cs, ARMMMUIdxBit_S1E2);
#endif
}

static void tlbimva_hyp_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    CPUState *cs = ENV_GET_CPU(env);
    uint64_t pageaddr = value & ~MAKE_64BIT_MASK(0, 12);

    tlb_flush_page_by_mmuidx(cs, pageaddr, ARMMMUIdxBit_S1E2);
}

static void tlbimva_hyp_is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                 uint64_t value)
{
  // Unicorn: if'd out. See issue 642.
#if 0
    CPUState *cs = ENV_GET_CPU(env);
    uint64_t pageaddr = value & ~MAKE_64BIT_MASK(0, 12);

    tlb_flush_page_by_mmuidx_all_cpus_synced(cs, pageaddr,
                                             ARMMMUIdxBit_S1E2);
#endif
}

static const ARMCPRegInfo cp_reginfo[] = {
    /* Define the secure and non-secure FCSE identifier CP registers
     * separately because there is no secure bank in V8 (no _EL3).  This allows
     * the secure register to be properly reset and migrated. There is also no
     * v8 EL1 version of the register so the non-secure instance stands alone.
     */
    { "FCSEIDR(NS)", 15,13,0, 0,0,0, 0,0,
      PL1_RW, ARM_CP_SECSTATE_NS, NULL, 0,
      offsetof(CPUARMState, cp15.fcseidr_ns), {0, 0},
      NULL, NULL, fcse_write, NULL, raw_write, },
    { "FCSEIDR(S)", 15,13,0, 0,0,0, 0,0,
      PL1_RW, ARM_CP_SECSTATE_S, NULL, 0,
      offsetof(CPUARMState, cp15.fcseidr_s), {0, 0},
      NULL, NULL, fcse_write, NULL, raw_write, },
    /* Define the secure and non-secure context identifier CP registers
     * separately because there is no secure bank in V8 (no _EL3).  This allows
     * the secure register to be properly reset and migrated.  In the
     * non-secure case, the 32-bit register will have reset and migration
     * disabled during registration as it is handled by the 64-bit instance.
     */
    { "CONTEXTIDR_EL1", 0,13,0, 3,0,1, ARM_CP_STATE_BOTH,
      0, PL1_RW, ARM_CP_SECSTATE_NS, NULL, 0, offsetof(CPUARMState, cp15.contextidr_el[1]), {0, 0},
      NULL, NULL, contextidr_write, NULL, raw_write, NULL, },
    { "CONTEXTIDR(S)", 15,13,0, 0,0,1, ARM_CP_STATE_AA32,0,
      PL1_RW, ARM_CP_SECSTATE_S, NULL, 0,
      offsetof(CPUARMState, cp15.contextidr_s), {0, 0},
      NULL, NULL, contextidr_write, NULL, raw_write, NULL, },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo not_v8_cp_reginfo[] = {
    /* NB: Some of these registers exist in v8 but with more precise
     * definitions that don't use CP_ANY wildcards (mostly in v8_cp_reginfo[]).
     */
    /* MMU Domain access control / MPU write buffer control */
    { "DACR", 15,3,CP_ANY, 0,CP_ANY,CP_ANY, 0,
      0, PL1_RW, 0, NULL, 0, 0,
      { offsetoflow32(CPUARMState, cp15.dacr_s), offsetoflow32(CPUARMState, cp15.dacr_ns) },
      NULL, NULL, dacr_write, NULL, raw_write, NULL, },
    /* ARMv7 allocates a range of implementation defined TLB LOCKDOWN regs.
     * For v6 and v5, these mappings are overly broad.
     */
    { "TLB_LOCKDOWN", 15,10,0, 0,CP_ANY,CP_ANY, 0,
      ARM_CP_NOP, PL1_RW,  },
    { "TLB_LOCKDOWN", 15,10,1, 0,CP_ANY,CP_ANY, 0,
      ARM_CP_NOP, PL1_RW,  },
    { "TLB_LOCKDOWN", 15,10,4, 0,CP_ANY,CP_ANY, 0,
      ARM_CP_NOP, PL1_RW,  },
    { "TLB_LOCKDOWN", 15,10,8, 0,CP_ANY,CP_ANY, 0,
      ARM_CP_NOP, PL1_RW,  },
    /* Cache maintenance ops; some of this space may be overridden later. */
    { "CACHEMAINT", 15,7,CP_ANY, 0,0,CP_ANY, 0,
      ARM_CP_NOP | ARM_CP_OVERRIDE, PL1_W, },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo not_v6_cp_reginfo[] = {
    /* Not all pre-v6 cores implemented this WFI, so this is slightly
     * over-broad.
     */
    { "WFI_v5", 15,7,8, 0,0,2, 0,
      ARM_CP_WFI, PL1_W, },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo not_v7_cp_reginfo[] = {
    /* Standard v6 WFI (also used in some pre-v6 cores); not in v7 (which
     * is UNPREDICTABLE; we choose to NOP as most implementations do).
     */
    { "WFI_v6", 15,7,0, 0,0,4, 0,
      ARM_CP_WFI, PL1_W, },
    /* L1 cache lockdown. Not architectural in v6 and earlier but in practice
     * implemented in 926, 946, 1026, 1136, 1176 and 11MPCore. StrongARM and
     * OMAPCP will override this space.
     */
    { "DLOCKDOWN", 15,9,0, 0,0,0, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c9_data), },
    { "ILOCKDOWN", 15,9,0, 0,0,1, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c9_insn), },
    /* v6 doesn't have the cache ID registers but Linux reads them anyway */
    { "DUMMY", 15,0,0, 0,1,CP_ANY, 0,
      ARM_CP_CONST | ARM_CP_NO_RAW, PL1_R, 0, NULL, 0 },
    /* We don't implement pre-v7 debug but most CPUs had at least a DBGDIDR;
     * implementing it as RAZ means the "debug architecture version" bits
     * will read as a reserved value, which should cause Linux to not try
     * to use the debug hardware.
     */
    { "DBGDIDR", 14,0,0, 0,0,0, 0,
      ARM_CP_CONST, PL0_R, 0, NULL, 0 },
    /* MMU TLB control. Note that the wildcarding means we cover not just
     * the unified TLB ops but also the dside/iside/inner-shareable variants.
     */
    { "TLBIALL", 15,8,CP_ANY, 0,CP_ANY,0, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiall_write, },
    { "TLBIMVA", 15,8,CP_ANY, 0,CP_ANY,1, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimva_write, },
    { "TLBIASID", 15,8,CP_ANY, 0,CP_ANY,2, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiasid_write, },
    { "TLBIMVAA", 15,8,CP_ANY, 0,CP_ANY,3, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimvaa_write, },
    { "PRRR", 15,10,2, 0,0,0, 0, ARM_CP_NOP,
      PL1_RW },
    { "NMRR", 15,10,2, 0,0,1, 0, ARM_CP_NOP,
      PL1_RW },
    REGINFO_SENTINEL
};

static void cpacr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                        uint64_t value)
{
    uint32_t mask = 0;

    /* In ARMv8 most bits of CPACR_EL1 are RES0. */
    if (!arm_feature(env, ARM_FEATURE_V8)) {
        /* ARMv7 defines bits for unimplemented coprocessors as RAZ/WI.
         * ASEDIS [31] and D32DIS [30] are both UNK/SBZP without VFP.
         * TRCDIS [28] is RAZ/WI since we do not implement a trace macrocell.
         */
        if (arm_feature(env, ARM_FEATURE_VFP)) {
            /* VFP coprocessor: cp10 & cp11 [23:20] */
            mask |= (1 << 31) | (1 << 30) | (0xf << 20);

            if (!arm_feature(env, ARM_FEATURE_NEON)) {
                /* ASEDIS [31] bit is RAO/WI */
                value |= (1 << 31);
            }

            /* VFPv3 and upwards with NEON implement 32 double precision
             * registers (D0-D31).
             */
            if (!arm_feature(env, ARM_FEATURE_NEON) ||
                    !arm_feature(env, ARM_FEATURE_VFP3)) {
                /* D32DIS [30] is RAO/WI if D16-31 are not implemented. */
                value |= (1 << 30);
            }
        }
        value &= mask;
    }
    env->cp15.cpacr_el1 = value;
}

static CPAccessResult cpacr_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                   bool isread)
{
    if (arm_feature(env, ARM_FEATURE_V8)) {
        /* Check if CPACR accesses are to be trapped to EL2 */
        if (arm_current_el(env) == 1 &&
            (env->cp15.cptr_el[2] & CPTR_TCPAC) && !arm_is_secure(env)) {
            return CP_ACCESS_TRAP_EL2;
        /* Check if CPACR accesses are to be trapped to EL3 */
        } else if (arm_current_el(env) < 3 &&
                   (env->cp15.cptr_el[3] & CPTR_TCPAC)) {
            return CP_ACCESS_TRAP_EL3;
        }
    }

    return CP_ACCESS_OK;
}

static CPAccessResult cptr_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                  bool isread)
{
    /* Check if CPTR accesses are set to trap to EL3 */
    if (arm_current_el(env) == 2 && (env->cp15.cptr_el[3] & CPTR_TCPAC)) {
        return CP_ACCESS_TRAP_EL3;
    }

    return CP_ACCESS_OK;
}

static const ARMCPRegInfo v6_cp_reginfo[] = {
    /* prefetch by MVA in v6, NOP in v7 */
    { "MVA_prefetch", 15,7,13, 0,0,1, 0,
      ARM_CP_NOP, PL1_W, },
    /* We need to break the TB after ISB to execute self-modifying code
     * correctly and also to take any pending interrupts immediately.
     * So use arm_cp_write_ignore() function instead of ARM_CP_NOP flag.
     */
    { "ISB", 15,7,5, 0,0,4, 0, ARM_CP_NO_RAW,
      PL0_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, arm_cp_write_ignore },
    { "DSB", 15,7,10, 0,0,4, 0,
      ARM_CP_NOP, PL0_W, },
    { "DMB", 15,7,10, 0,0,5, 0,
      ARM_CP_NOP, PL0_W, },
    { "IFAR", 15,6,0, 0,0,2, 0,
      0, PL1_RW, 0, NULL, 0, 0,
      { offsetof(CPUARMState, cp15.ifar_s), offsetof(CPUARMState, cp15.ifar_ns) } },
    /* Watchpoint Fault Address Register : should actually only be present
     * for 1136, 1176, 11MPCore.
     */
    { "WFAR", 15,6,0, 0,0,1, 0,
      ARM_CP_CONST, PL1_RW, 0, NULL, 0, },
    { "CPACR", 0,1,0, 3,0,2, ARM_CP_STATE_BOTH,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.cpacr_el1), {0, 0},
      cpacr_access, NULL, cpacr_write },
    REGINFO_SENTINEL
};

static CPAccessResult pmreg_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                   bool isread)
{
    /* Performance monitor registers user accessibility is controlled
     * by PMUSERENR. MDCR_EL2.TPM and MDCR_EL3.TPM allow configurable
     * trapping to EL2 or EL3 for other accesses.
     */
    int el = arm_current_el(env);

    if (el == 0 && !(env->cp15.c9_pmuserenr & 1)) {
        return CP_ACCESS_TRAP;
    }
    if (el < 2 && (env->cp15.mdcr_el2 & MDCR_TPM)
        && !arm_is_secure_below_el3(env)) {
        return CP_ACCESS_TRAP_EL2;
    }
    if (el < 3 && (env->cp15.mdcr_el3 & MDCR_TPM)) {
        return CP_ACCESS_TRAP_EL3;
    }

    return CP_ACCESS_OK;
}

static CPAccessResult pmreg_access_xevcntr(CPUARMState *env,
                                           const ARMCPRegInfo *ri,
                                           bool isread)
{
    /* ER: event counter read trap control */
    if (arm_feature(env, ARM_FEATURE_V8)
        && arm_current_el(env) == 0
        && (env->cp15.c9_pmuserenr & (1 << 3)) != 0
        && isread) {
        return CP_ACCESS_OK;
    }

    return pmreg_access(env, ri, isread);
}

static CPAccessResult pmreg_access_swinc(CPUARMState *env,
                                         const ARMCPRegInfo *ri,
                                         bool isread)
{
    /* SW: software increment write trap control */
    if (arm_feature(env, ARM_FEATURE_V8)
        && arm_current_el(env) == 0
        && (env->cp15.c9_pmuserenr & (1 << 1)) != 0
        && !isread) {
        return CP_ACCESS_OK;
    }

    return pmreg_access(env, ri, isread);
}

#ifndef CONFIG_USER_ONLY

static CPAccessResult pmreg_access_selr(CPUARMState *env,
                                        const ARMCPRegInfo *ri,
                                        bool isread)
{
    /* ER: event counter read trap control */
    if (arm_feature(env, ARM_FEATURE_V8)
        && arm_current_el(env) == 0
        && (env->cp15.c9_pmuserenr & (1 << 3)) != 0) {
        return CP_ACCESS_OK;
    }

    return pmreg_access(env, ri, isread);
}

static CPAccessResult pmreg_access_ccntr(CPUARMState *env,
                                         const ARMCPRegInfo *ri,
                                         bool isread)
{
    /* CR: cycle counter read trap control */
    if (arm_feature(env, ARM_FEATURE_V8)
        && arm_current_el(env) == 0
        && (env->cp15.c9_pmuserenr & (1 << 2)) != 0
        && isread) {
        return CP_ACCESS_OK;
    }

    return pmreg_access(env, ri, isread);
}

static inline bool arm_ccnt_enabled(CPUARMState *env)
{
    /* This does not support checking PMCCFILTR_EL0 register */

    if (!(env->cp15.c9_pmcr & PMCRE)) {
        return false;
    }

    return true;
}

void pmccntr_sync(CPUARMState *env)
{
    uint64_t temp_ticks;

    temp_ticks = muldiv64(qemu_clock_get_us(QEMU_CLOCK_VIRTUAL),
                          NANOSECONDS_PER_SECOND, 1000000);

    if (env->cp15.c9_pmcr & PMCRD) {
        /* Increment once every 64 processor clock cycles */
        temp_ticks /= 64;
    }

    if (arm_ccnt_enabled(env)) {
        env->cp15.c15_ccnt = temp_ticks - env->cp15.c15_ccnt;
    }
}

static void pmcr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                       uint64_t value)
{
    pmccntr_sync(env);

    if (value & PMCRC) {
        /* The counter has been reset */
        env->cp15.c15_ccnt = 0;
    }

    /* only the DP, X, D and E bits are writable */
    env->cp15.c9_pmcr &= ~0x39;
    env->cp15.c9_pmcr |= (value & 0x39);

    pmccntr_sync(env);
}

static uint64_t pmccntr_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    uint64_t total_ticks;

    if (!arm_ccnt_enabled(env)) {
        /* Counter is disabled, do not change value */
        return env->cp15.c15_ccnt;
    }

    total_ticks = muldiv64(qemu_clock_get_us(QEMU_CLOCK_VIRTUAL),
                           NANOSECONDS_PER_SECOND, 1000000);

    if (env->cp15.c9_pmcr & PMCRD) {
        /* Increment once every 64 processor clock cycles */
        total_ticks /= 64;
    }
    return total_ticks - env->cp15.c15_ccnt;
}

static void pmselr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value)
{
    /* The value of PMSELR.SEL affects the behavior of PMXEVTYPER and
     * PMXEVCNTR. We allow [0..31] to be written to PMSELR here; in the
     * meanwhile, we check PMSELR.SEL when PMXEVTYPER and PMXEVCNTR are
     * accessed.
     */
    env->cp15.c9_pmselr = value & 0x1f;
}

static void pmccntr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                        uint64_t value)
{
    uint64_t total_ticks;

    if (!arm_ccnt_enabled(env)) {
        /* Counter is disabled, set the absolute value */
        env->cp15.c15_ccnt = value;
        return;
    }

    total_ticks = muldiv64(qemu_clock_get_us(QEMU_CLOCK_VIRTUAL),
                           NANOSECONDS_PER_SECOND, 1000000);

    if (env->cp15.c9_pmcr & PMCRD) {
        /* Increment once every 64 processor clock cycles */
        total_ticks /= 64;
    }
    env->cp15.c15_ccnt = total_ticks - value;
}

static void pmccntr_write32(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    uint64_t cur_val = pmccntr_read(env, NULL);

    pmccntr_write(env, ri, deposit64(cur_val, 0, 32, value));
}

#else /* CONFIG_USER_ONLY */

void pmccntr_sync(CPUARMState *env)
{
}

#endif

static void pmccfiltr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    pmccntr_sync(env);
    env->cp15.pmccfiltr_el0 = value & 0x7E000000;
    pmccntr_sync(env);
}

static void pmcntenset_write(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    value &= (1 << 31);
    env->cp15.c9_pmcnten |= value;
}

static void pmcntenclr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t value)
{
    value &= (1 << 31);
    env->cp15.c9_pmcnten &= ~value;
}

static void pmovsr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value)
{
    env->cp15.c9_pmovsr &= ~value;
}

static void pmxevtyper_write(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t value)
{
    /* Attempts to access PMXEVTYPER are CONSTRAINED UNPREDICTABLE when
     * PMSELR value is equal to or greater than the number of implemented
     * counters, but not equal to 0x1f. We opt to behave as a RAZ/WI.
     */
    if (env->cp15.c9_pmselr == 0x1f) {
        pmccfiltr_write(env, ri, value);
    }
}

static uint64_t pmxevtyper_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    /* We opt to behave as a RAZ/WI when attempts to access PMXEVTYPER
     * are CONSTRAINED UNPREDICTABLE. See comments in pmxevtyper_write().
     */
    if (env->cp15.c9_pmselr == 0x1f) {
        return env->cp15.pmccfiltr_el0;
    } else {
        return 0;
    }
}

static void pmuserenr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    if (arm_feature(env, ARM_FEATURE_V8)) {
        env->cp15.c9_pmuserenr = value & 0xf;
    } else {
        env->cp15.c9_pmuserenr = value & 1;
    }
}

static void pmintenset_write(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t value)
{
    /* We have no event counters so only the C bit can be changed */
    value &= (1 << 31);
    env->cp15.c9_pminten |= value;
}

static void pmintenclr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t value)
{
    value &= (1 << 31);
    env->cp15.c9_pminten &= ~value;
}

static void vbar_write(CPUARMState *env, const ARMCPRegInfo *ri,
                       uint64_t value)
{
    /* Note that even though the AArch64 view of this register has bits
     * [10:0] all RES0 we can only mask the bottom 5, to comply with the
     * architectural requirements for bits which are RES0 only in some
     * contexts. (ARMv8 would permit us to do no masking at all, but ARMv7
     * requires the bottom five bits to be RAZ/WI because they're UNK/SBZP.)
     */
    raw_write(env, ri, value & ~0x1FULL);
}

static void scr_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    /* We only mask off bits that are RES0 both for AArch64 and AArch32.
     * For bits that vary between AArch32/64, code needs to check the
     * current execution mode before directly using the feature bit.
     */
    uint32_t valid_mask = SCR_AARCH64_MASK | SCR_AARCH32_MASK;

    if (!arm_feature(env, ARM_FEATURE_EL2)) {
        valid_mask &= ~SCR_HCE;

        /* On ARMv7, SMD (or SCD as it is called in v7) is only
         * supported if EL2 exists. The bit is UNK/SBZP when
         * EL2 is unavailable. In QEMU ARMv7, we force it to always zero
         * when EL2 is unavailable.
         * On ARMv8, this bit is always available.
         */
        if (arm_feature(env, ARM_FEATURE_V7) &&
            !arm_feature(env, ARM_FEATURE_V8)) {
            valid_mask &= ~SCR_SMD;
        }
    }

    /* Clear all-context RES0 bits.  */
    value &= valid_mask;
    raw_write(env, ri, value);
}

static uint64_t ccsidr_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    /* Acquire the CSSELR index from the bank corresponding to the CCSIDR
     * bank
     */
    uint32_t index = A32_BANKED_REG_GET(env, csselr,
                                        ri->secure & ARM_CP_SECSTATE_S);

    return cpu->ccsidr[index];
}

static void csselr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value)
{
    raw_write(env, ri, value & 0xf);
}

static uint64_t isr_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    CPUState *cs = ENV_GET_CPU(env);
    uint64_t ret = 0;

    if (cs->interrupt_request & CPU_INTERRUPT_HARD) {
        ret |= CPSR_I;
    }
    if (cs->interrupt_request & CPU_INTERRUPT_FIQ) {
        ret |= CPSR_F;
    }
    /* External aborts are not possible in QEMU so A bit is always clear */
    return ret;
}

static const ARMCPRegInfo v7_cp_reginfo[] = {
    /* the old v6 WFI, UNPREDICTABLE in v7 but we choose to NOP */
    { "NOP", 15,7,0, 0,0,4, 0,
      ARM_CP_NOP, PL1_W,  },
    /* Performance monitors are implementation defined in v7,
     * but with an ARM recommended set of registers, which we
     * follow (although we don't actually implement any counters)
     *
     * Performance registers fall into three categories:
     *  (a) always UNDEF in PL0, RW in PL1 (PMINTENSET, PMINTENCLR)
     *  (b) RO in PL0 (ie UNDEF on write), RW in PL1 (PMUSERENR)
     *  (c) UNDEF in PL0 if PMUSERENR.EN==0, otherwise accessible (all others)
     * For the cases controlled by PMUSERENR we must set .access to PL0_RW
     * or PL0_RO as appropriate and then check PMUSERENR in the helper fn.
     */
    { "PMCNTENSET", 15,9,12, 0,0,1, 0,
      ARM_CP_ALIAS, PL0_RW, 0, NULL, 0, offsetoflow32(CPUARMState, cp15.c9_pmcnten), {0, 0},
      pmreg_access, NULL, pmcntenset_write, NULL, raw_write },
    { "PMCNTENSET_EL0", 0,9,12, 3,3,1, ARM_CP_STATE_AA64,
      0, PL0_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c9_pmcnten), {0, 0},
      pmreg_access, NULL, pmcntenset_write, NULL, raw_write },
    { "PMCNTENCLR", 15,9,12, 0,0,2, 0,
      ARM_CP_ALIAS, PL0_RW, 0, NULL, 0, offsetoflow32(CPUARMState, cp15.c9_pmcnten), {0, 0},
      pmreg_access, NULL, pmcntenclr_write, },
    { "PMCNTENCLR_EL0", 0,9,12, 3,3,2, ARM_CP_STATE_AA64,
      ARM_CP_ALIAS, PL0_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c9_pmcnten), {0, 0},
      pmreg_access, NULL, pmcntenclr_write },
    { "PMOVSR", 15,9,12, 0,0,3, 0,
      0, PL0_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c9_pmovsr), {0, 0},
      pmreg_access, NULL, pmovsr_write, NULL, raw_write },
    { "PMOVSCLR_EL0", 0,9,12, 3,3,3, ARM_CP_STATE_AA64, ARM_CP_ALIAS,
      PL0_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c9_pmovsr), {0, 0},
      pmreg_access, NULL, pmovsr_write, NULL, raw_write },
    /* Unimplemented so WI. */
    { "PMSWINC", 15,9,12, 0,0,4, 0,
      ARM_CP_NOP, PL0_W, 0, NULL, 0, 0, {0, 0},
      pmreg_access_swinc },
#ifndef CONFIG_USER_ONLY
    { "PMSELR", 15,9,12, 0,0,5, 0, ARM_CP_ALIAS,
      PL0_RW, 0, NULL, 0, offsetoflow32(CPUARMState, cp15.c9_pmselr), {0, 0},
      pmreg_access_selr, NULL, pmselr_write, NULL, raw_write},
    { "PMSELR_EL0", 0,9,12, 3,3,5, ARM_CP_STATE_AA64, 0,
      PL0_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c9_pmselr), {0, 0},
      pmreg_access_selr, NULL, pmselr_write, NULL, raw_write, },
    { "PMCCNTR", 15,9,13, 0,0,0, 0,
      ARM_CP_IO, PL0_RW, 0, NULL, 0, 0, {0, 0},
      pmreg_access_ccntr, pmccntr_read, pmccntr_write32, },
    { "PMCCNTR_EL0", 0,9,13, 3,3,0, ARM_CP_STATE_AA64,
      ARM_CP_IO, PL0_RW, 0, NULL, 0, 0, {0, 0},
      pmreg_access_ccntr, pmccntr_read, pmccntr_write, },
#endif
    { "PMCCFILTR_EL0", 0,14,15, 3,3,7, ARM_CP_STATE_AA64,
      ARM_CP_IO, PL0_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.pmccfiltr_el0), {0, 0},
      pmreg_access, NULL, pmccfiltr_write, },
    { "PMXEVTYPER", 15,9,13, 0,0,1, 0, ARM_CP_NO_RAW,
      PL0_RW, 0, NULL, 0, 0, {0, 0},
      pmreg_access, pmxevtyper_read, pmxevtyper_write },
    { "PMXEVTYPER_EL0", 0,9,13, 3,3,1, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL0_RW, 0, NULL, 0, 0, {0, 0},
      pmreg_access, pmxevtyper_read, pmxevtyper_write },
    /* Unimplemented, RAZ/WI. */
    { "PMXEVCNTR", 15,9,13, 0,0,2, 0,
      ARM_CP_CONST, PL0_RW, 0, NULL, 0, 0, {0, 0},
      pmreg_access_xevcntr },
    { "PMUSERENR", 15,9,14, 0,0,0, 0,
      0, PL0_R | PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c9_pmuserenr), {0, 0},
      access_tpm, NULL, pmuserenr_write, NULL, raw_write },
    { "PMUSERENR_EL0", 0,9,14,3,3,0, ARM_CP_STATE_AA64, ARM_CP_ALIAS,
      PL0_R | PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c9_pmuserenr), {0, 0},
      access_tpm, NULL, pmuserenr_write, NULL, raw_write },
    { "PMINTENSET", 15,9,14, 0,0,1, 0, ARM_CP_ALIAS,
      PL1_RW, 0, NULL, 0, offsetoflow32(CPUARMState, cp15.c9_pminten), {0, 0},
      access_tpm, NULL, pmintenset_write, NULL, raw_write },
    { "PMINTENSET_EL1", 0,9,14, 3,0,1, ARM_CP_STATE_AA64, ARM_CP_IO,
      PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c9_pminten), {0, 0},
      access_tpm, NULL, pmintenset_write, NULL, raw_write },
    { "PMINTENCLR", 15,9,14, 0,0,2, 0,
      ARM_CP_ALIAS, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c9_pminten), {0, 0},
      access_tpm, NULL, pmintenclr_write, },
    { "PMINTENCLR_EL1", 0,9,14, 3,0,2, ARM_CP_STATE_AA64, ARM_CP_ALIAS,
      PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c9_pminten), {0, 0},
      access_tpm, NULL, pmintenclr_write },
    { "CCSIDR", 0,0,0, 3,1,0, ARM_CP_STATE_BOTH,
      ARM_CP_NO_RAW, PL1_R, 0, NULL, 0, 0, {0, 0},
      NULL, ccsidr_read, },
    { "CSSELR", 0,0,0, 3,2,0, ARM_CP_STATE_BOTH,
      0, PL1_RW, 0, NULL, 0, 0,
      { offsetof(CPUARMState, cp15.csselr_s), offsetof(CPUARMState, cp15.csselr_ns) },
      NULL, NULL, csselr_write, },
    /* Auxiliary ID register: this actually has an IMPDEF value but for now
     * just RAZ for all cores:
     */
    { "AIDR", 0,0,0, 3,1,7, ARM_CP_STATE_BOTH,
      ARM_CP_CONST, PL1_R, 0, NULL, 0 },
    /* Auxiliary fault status registers: these also are IMPDEF, and we
     * choose to RAZ/WI for all cores.
     */
    { "AFSR0_EL1", 0,5,1, 3,0,0, ARM_CP_STATE_BOTH,
      ARM_CP_CONST, PL1_RW, 0, NULL, 0 },
    { "AFSR1_EL1", 0,5,1, 3,0,1, ARM_CP_STATE_BOTH,
      ARM_CP_CONST, PL1_RW, 0, NULL, 0 },
    /* MAIR can just read-as-written because we don't implement caches
     * and so don't need to care about memory attributes.
     */
    { "MAIR_EL1", 0,10,2, 3,0,0, ARM_CP_STATE_AA64,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.mair_el[1]), },
    { "MAIR_EL3", 0,10,2, 3,6,0, ARM_CP_STATE_AA64, 0,
      PL3_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.mair_el[3]) },
    /* For non-long-descriptor page tables these are PRRR and NMRR;
     * regardless they still act as reads-as-written for QEMU.
     */
     /* MAIR0/1 are defined separately from their 64-bit counterpart which
      * allows them to assign the correct fieldoffset based on the endianness
      * handled in the field definitions.
      */
    { "MAIR0", 15,10,2, 0,0,0, ARM_CP_STATE_AA32, 0,
      PL1_RW, 0, NULL, 0, 0,
      { offsetof(CPUARMState, cp15.mair0_s), offsetof(CPUARMState, cp15.mair0_ns) },
      NULL, NULL, NULL, NULL, NULL, arm_cp_reset_ignore },
    { "MAIR1", 15,10,2, 0,0,1, ARM_CP_STATE_AA32, 0,
      PL1_RW, 0, NULL, 0, 0,
      { offsetof(CPUARMState, cp15.mair1_s), offsetof(CPUARMState, cp15.mair1_ns) },
      NULL, NULL, NULL, NULL, NULL, arm_cp_reset_ignore },
    { "ISR_EL1", 0,12,1, 3,0,0, ARM_CP_STATE_BOTH,
      ARM_CP_NO_RAW, PL1_R, 0, NULL, 0, 0, {0, 0},
      NULL, isr_read },
    /* 32 bit ITLB invalidates */
    { "ITLBIALL", 15,8,5, 0,0,0, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiall_write },
    { "ITLBIMVA", 15,8,5, 0,0,1, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimva_write },
    { "ITLBIASID", 15,8,5, 0,0,2, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiasid_write },
    /* 32 bit DTLB invalidates */
    { "DTLBIALL", 15,8,6, 0,0,0, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiall_write },
    { "DTLBIMVA", 15,8,6, 0,0,1, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimva_write },
    { "DTLBIASID", 15,8,6, 0,0,2, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiasid_write },
    /* 32 bit TLB invalidates */
    { "TLBIALL", 15,8,7, 0,0,0, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiall_write },
    { "TLBIMVA", 15,8,7, 0,0,1, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimva_write },
    { "TLBIASID", 15,8,7, 0,0,2, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiasid_write },
    { "TLBIMVAA", 15,8,7, 0,0,3, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimvaa_write },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo v7mp_cp_reginfo[] = {
    /* 32 bit TLB invalidates, Inner Shareable */
    { "TLBIALLIS", 15,8,3, 0,0,0, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiall_is_write },
    { "TLBIMVAIS", 15,8,3, 0,0,1, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimva_is_write },
    { "TLBIASIDIS", 15,8,3, 0,0,2, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiasid_is_write },
    { "TLBIMVAAIS", 15,8,3, 0,0,3, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimvaa_is_write },
    REGINFO_SENTINEL
};

static void teecr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                        uint64_t value)
{
    value &= 1;
    env->teecr = value;
}

static CPAccessResult teehbr_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                    bool isread)
{
    if (arm_current_el(env) == 0 && (env->teecr & 1)) {
        return CP_ACCESS_TRAP;
    }
    return CP_ACCESS_OK;
}

static const ARMCPRegInfo t2ee_cp_reginfo[] = {
    { "TEECR", 14,0,0, 0,6,0, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, teecr), {0, 0},
      NULL, NULL, teecr_write },
    { "TEEHBR", 14,1,0, 0,6,0, 0,
      0, PL0_RW, 0, NULL, 0, offsetof(CPUARMState, teehbr), {0, 0},
      teehbr_access, },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo v6k_cp_reginfo[] = {
    { "TPIDR_EL0", 0,13,0, 3,3,2, ARM_CP_STATE_AA64,
      0, PL0_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.tpidr_el[0]), },
    { "TPIDRURW", 15,13,0, 0,0,2, 0,
      0, PL0_RW, 0, NULL, 0, 0,
      { offsetoflow32(CPUARMState, cp15.tpidrurw_s), offsetoflow32(CPUARMState, cp15.tpidrurw_ns) },
      NULL, NULL, NULL, NULL, NULL, arm_cp_reset_ignore },
    { "TPIDRRO_EL0", 0,13,0, 3,3,3, ARM_CP_STATE_AA64,
      0, PL0_R|PL1_W, 0, NULL, 0, offsetof(CPUARMState, cp15.tpidrro_el[0]) },
    { "TPIDRURO", 15,13,0, 0,0,3, 0,
      0, PL0_R|PL1_W, 0, NULL, 0, 0,
      {offsetoflow32(CPUARMState, cp15.tpidruro_s), offsetoflow32(CPUARMState, cp15.tpidruro_ns) },
      NULL, NULL, NULL, NULL, NULL, arm_cp_reset_ignore },
    { "TPIDR_EL1", 0,13,0, 3,0,4, ARM_CP_STATE_AA64,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.tpidr_el[1]) },
    { "TPIDRPRW", 15,13,0, 0,0,4, 0,0,
      PL1_RW, 0, NULL, 0,0,
      { offsetoflow32(CPUARMState, cp15.tpidrprw_s), offsetoflow32(CPUARMState, cp15.tpidrprw_ns)} },
    REGINFO_SENTINEL
};

#ifndef CONFIG_USER_ONLY

static CPAccessResult gt_cntfrq_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                       bool isread)
{
    /* CNTFRQ: not visible from PL0 if both PL0PCTEN and PL0VCTEN are zero.
     * Writable only at the highest implemented exception level.
     */
    int el = arm_current_el(env);

    switch (el) {
    case 0:
        if (!extract32(env->cp15.c14_cntkctl, 0, 2)) {
            return CP_ACCESS_TRAP;
        }
        break;
    case 1:
        if (!isread && ri->state == ARM_CP_STATE_AA32 &&
            arm_is_secure_below_el3(env)) {
            /* Accesses from 32-bit Secure EL1 UNDEF (*not* trap to EL3!) */
            return CP_ACCESS_TRAP_UNCATEGORIZED;
        }
        break;
    case 2:
    case 3:
        break;
    }

    if (!isread && el < arm_highest_el(env)) {
        return CP_ACCESS_TRAP_UNCATEGORIZED;
    }

    return CP_ACCESS_OK;
}

static CPAccessResult gt_counter_access(CPUARMState *env, int timeridx,
                                        bool isread)
{
    unsigned int cur_el = arm_current_el(env);
    bool secure = arm_is_secure(env);

    /* CNT[PV]CT: not visible from PL0 if ELO[PV]CTEN is zero */
    if (cur_el == 0 &&
        !extract32(env->cp15.c14_cntkctl, timeridx, 1)) {
        return CP_ACCESS_TRAP;
    }

    if (arm_feature(env, ARM_FEATURE_EL2) &&
        timeridx == GTIMER_PHYS && !secure && cur_el < 2 &&
        !extract32(env->cp15.cnthctl_el2, 0, 1)) {
        return CP_ACCESS_TRAP_EL2;
    }
    return CP_ACCESS_OK;
}

static CPAccessResult gt_timer_access(CPUARMState *env, int timeridx,
                                      bool isread)
{
    unsigned int cur_el = arm_current_el(env);
    bool secure = arm_is_secure(env);

    /* CNT[PV]_CVAL, CNT[PV]_CTL, CNT[PV]_TVAL: not visible from PL0 if
     * EL0[PV]TEN is zero.
     */
    if (cur_el == 0 &&
        !extract32(env->cp15.c14_cntkctl, 9 - timeridx, 1)) {
        return CP_ACCESS_TRAP;
    }

    if (arm_feature(env, ARM_FEATURE_EL2) &&
        timeridx == GTIMER_PHYS && !secure && cur_el < 2 &&
        !extract32(env->cp15.cnthctl_el2, 1, 1)) {
        return CP_ACCESS_TRAP_EL2;
    }
    return CP_ACCESS_OK;
}

static CPAccessResult gt_pct_access(CPUARMState *env,
                                    const ARMCPRegInfo *ri,
                                    bool isread)
{
    return gt_counter_access(env, GTIMER_PHYS, isread);
}

static CPAccessResult gt_vct_access(CPUARMState *env,
                                    const ARMCPRegInfo *ri,
                                    bool isread)
{
    return gt_counter_access(env, GTIMER_VIRT, isread);
}

static CPAccessResult gt_ptimer_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                       bool isread)
{
    return gt_timer_access(env, GTIMER_PHYS, isread);
}

static CPAccessResult gt_vtimer_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                       bool isread)
{
    return gt_timer_access(env, GTIMER_VIRT, isread);
}

static CPAccessResult gt_stimer_access(CPUARMState *env,
                                       const ARMCPRegInfo *ri,
                                       bool isread)
{
    /* The AArch64 register view of the secure physical timer is
     * always accessible from EL3, and configurably accessible from
     * Secure EL1.
     */
    switch (arm_current_el(env)) {
    case 1:
        if (!arm_is_secure(env)) {
            return CP_ACCESS_TRAP;
        }
        if (!(env->cp15.scr_el3 & SCR_ST)) {
            return CP_ACCESS_TRAP_EL3;
        }
        return CP_ACCESS_OK;
    case 0:
    case 2:
        return CP_ACCESS_TRAP;
    case 3:
        return CP_ACCESS_OK;
    default:
        g_assert_not_reached();
    }
}

static uint64_t gt_get_countervalue(CPUARMState *env)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / GTIMER_SCALE;
}

static void gt_recalc_timer(ARMCPU *cpu, int timeridx)
{
    ARMGenericTimer *gt = &cpu->env.cp15.c14_timer[timeridx];

    if (gt->ctl & 1) {
        /* Timer enabled: calculate and set current ISTATUS, irq, and
         * reset timer to when ISTATUS next has to change
         */
        uint64_t offset = timeridx == GTIMER_VIRT ?
                                      cpu->env.cp15.cntvoff_el2 : 0;
        uint64_t count = gt_get_countervalue(&cpu->env);
        /* Note that this must be unsigned 64 bit arithmetic: */
        int istatus = count - offset >= gt->cval;
        uint64_t nexttick;
        //int irqstate;

        gt->ctl = deposit32(gt->ctl, 2, 1, istatus);

        // Unicorn: commented out
        //irqstate = (istatus && !(gt->ctl & 2));
        //qemu_set_irq(cpu->gt_timer_outputs[timeridx], irqstate);

        if (istatus) {
            /* Next transition is when count rolls back over to zero */
            nexttick = UINT64_MAX;
        } else {
            /* Next transition is when we hit cval */
            nexttick = gt->cval + offset;
        }
        /* Note that the desired next expiry time might be beyond the
         * signed-64-bit range of a QEMUTimer -- in this case we just
         * set the timer for as far in the future as possible. When the
         * timer expires we will reset the timer for any remaining period.
         */
        if (nexttick > INT64_MAX / GTIMER_SCALE) {
            nexttick = INT64_MAX / GTIMER_SCALE;
        }
        // Unicorn: commented out
        //timer_mod(cpu->gt_timer[timeridx], nexttick);
        //trace_arm_gt_recalc(timeridx, irqstate, nexttick);
    } else {
        /* Timer disabled: ISTATUS and timer output always clear */
        gt->ctl &= ~4;
        // Unicorn: commented out
        //qemu_set_irq(cpu->gt_timer_outputs[timeridx], 0);
        //timer_del(cpu->gt_timer[timeridx]);
        //trace_arm_gt_recalc_disabled(timeridx);
    }
}

static void gt_timer_reset(CPUARMState *env, const ARMCPRegInfo *ri,
                           int timeridx)
{
}

static uint64_t gt_cnt_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return gt_get_countervalue(env);
}

static uint64_t gt_virt_cnt_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return gt_get_countervalue(env) - env->cp15.cntvoff_el2;
}

static void gt_cval_write(CPUARMState *env, const ARMCPRegInfo *ri,
                          int timeridx,
                          uint64_t value)
{
    // Unicorn: commented out
    //trace_arm_gt_cval_write(timeridx, value);
    env->cp15.c14_timer[timeridx].cval = value;
    //gt_recalc_timer(arm_env_get_cpu(env), timeridx);
}

static uint64_t gt_tval_read(CPUARMState *env, const ARMCPRegInfo *ri,
                             int timeridx)
{
    uint64_t offset = timeridx == GTIMER_VIRT ? env->cp15.cntvoff_el2 : 0;

    return (uint32_t)(env->cp15.c14_timer[timeridx].cval -
                      (gt_get_countervalue(env) - offset));
}

static void gt_tval_write(CPUARMState *env, const ARMCPRegInfo *ri,
                          int timeridx,
                          uint64_t value)
{
    uint64_t offset = timeridx == GTIMER_VIRT ? env->cp15.cntvoff_el2 : 0;

    // Unicorn: commented out
    //trace_arm_gt_tval_write(timeridx, value);
    env->cp15.c14_timer[timeridx].cval = gt_get_countervalue(env) - offset +
                                         sextract64(value, 0, 32);
    gt_recalc_timer(arm_env_get_cpu(env), timeridx);
}

static void gt_ctl_write(CPUARMState *env, const ARMCPRegInfo *ri,
                         int timeridx,
                         uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    uint32_t oldval = env->cp15.c14_timer[timeridx].ctl;

    // Unicorn: commented out
    //trace_arm_gt_ctl_write(timeridx, value);
    env->cp15.c14_timer[timeridx].ctl = deposit64(oldval, 0, 2, value);
    if ((oldval ^ value) & 1) {
        /* Enable toggled */
        gt_recalc_timer(cpu, timeridx);
    } else if ((oldval ^ value) & 2) {
        /* IMASK toggled: don't need to recalculate,
         * just set the interrupt line based on ISTATUS
         */
        /* Unicorn: commented out
        int irqstate = (oldval & 4) && !(value & 2);

        trace_arm_gt_imask_toggle(timeridx, irqstate);
        qemu_set_irq(cpu->gt_timer_outputs[timeridx], irqstate);
        */
    }
}

static void gt_phys_timer_reset(CPUARMState *env, const ARMCPRegInfo *ri)
{
    gt_timer_reset(env, ri, GTIMER_PHYS);
}

static void gt_phys_cval_write(CPUARMState *env, const ARMCPRegInfo *ri,
                               uint64_t value)
{
    gt_cval_write(env, ri, GTIMER_PHYS, value);
}

static uint64_t gt_phys_tval_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return gt_tval_read(env, ri, GTIMER_PHYS);
}

static void gt_phys_tval_write(CPUARMState *env, const ARMCPRegInfo *ri,
                               uint64_t value)
{
    gt_tval_write(env, ri, GTIMER_PHYS, value);
}

static void gt_phys_ctl_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    gt_ctl_write(env, ri, GTIMER_PHYS, value);
}

static void gt_virt_timer_reset(CPUARMState *env, const ARMCPRegInfo *ri)
{
    gt_timer_reset(env, ri, GTIMER_VIRT);
}

static void gt_virt_cval_write(CPUARMState *env, const ARMCPRegInfo *ri,
                               uint64_t value)
{
    gt_cval_write(env, ri, GTIMER_VIRT, value);
}

static uint64_t gt_virt_tval_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return gt_tval_read(env, ri, GTIMER_VIRT);
}

static void gt_virt_tval_write(CPUARMState *env, const ARMCPRegInfo *ri,
                               uint64_t value)
{
    gt_tval_write(env, ri, GTIMER_VIRT, value);
}

static void gt_virt_ctl_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    gt_ctl_write(env, ri, GTIMER_VIRT, value);
}

static void gt_cntvoff_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    // Unicorn: commented out
    //trace_arm_gt_cntvoff_write(value);
    raw_write(env, ri, value);
    gt_recalc_timer(cpu, GTIMER_VIRT);
}

static void gt_hyp_timer_reset(CPUARMState *env, const ARMCPRegInfo *ri)
{
    gt_timer_reset(env, ri, GTIMER_HYP);
}

static void gt_hyp_cval_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    gt_cval_write(env, ri, GTIMER_HYP, value);
}

static uint64_t gt_hyp_tval_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return gt_tval_read(env, ri, GTIMER_HYP);
}

static void gt_hyp_tval_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    gt_tval_write(env, ri, GTIMER_HYP, value);
}

static void gt_hyp_ctl_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    gt_ctl_write(env, ri, GTIMER_HYP, value);
}

static void gt_sec_timer_reset(CPUARMState *env, const ARMCPRegInfo *ri)
{
    gt_timer_reset(env, ri, GTIMER_SEC);
}

static void gt_sec_cval_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    gt_cval_write(env, ri, GTIMER_SEC, value);
}

static uint64_t gt_sec_tval_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return gt_tval_read(env, ri, GTIMER_SEC);
}

static void gt_sec_tval_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    gt_tval_write(env, ri, GTIMER_SEC, value);
}

static void gt_sec_ctl_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    gt_ctl_write(env, ri, GTIMER_SEC, value);
}

void arm_gt_ptimer_cb(void *opaque)
{
    ARMCPU *cpu = opaque;

    gt_recalc_timer(cpu, GTIMER_PHYS);
}

void arm_gt_vtimer_cb(void *opaque)
{
    ARMCPU *cpu = opaque;

    gt_recalc_timer(cpu, GTIMER_VIRT);
}

void arm_gt_htimer_cb(void *opaque)
{
    ARMCPU *cpu = opaque;

    gt_recalc_timer(cpu, GTIMER_HYP);
}

void arm_gt_stimer_cb(void *opaque)
{
    ARMCPU *cpu = opaque;

    gt_recalc_timer(cpu, GTIMER_SEC);
}

static const ARMCPRegInfo generic_timer_cp_reginfo[] = {
    /* Note that CNTFRQ is purely reads-as-written for the benefit
     * of software; writing it doesn't actually change the timer frequency.
     * Our reset value matches the fixed frequency we implement the timer at.
     */
    { "CNTFRQ", 15,14,0, 0,0,0, 0,
      ARM_CP_ALIAS, PL1_RW | PL0_R, 0, NULL, 0, offsetoflow32(CPUARMState, cp15.c14_cntfrq), {0, 0},
      gt_cntfrq_access, NULL, NULL, NULL, NULL, NULL },
    { "CNTFRQ_EL0", 0,14,0, 3,3,0, ARM_CP_STATE_AA64,
      0, PL1_RW | PL0_R, 0, NULL, (1000 * 1000 * 1000) / GTIMER_SCALE, offsetof(CPUARMState, cp15.c14_cntfrq), {0, 0},
      gt_cntfrq_access, },
    /* overall control: mostly access permissions */
    { "CNTKCTL", 0,14,1, 3,0,0, ARM_CP_STATE_BOTH,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c14_cntkctl), },
    /* per-timer control */
    { "CNTP_CTL", 15,14,2, 0,0,1, 0,
      ARM_CP_IO | ARM_CP_ALIAS, PL1_RW | PL0_R, ARM_CP_SECSTATE_NS, NULL, 0, offsetoflow32(CPUARMState, cp15.c14_timer[GTIMER_PHYS].ctl), {0, 0},
      gt_ptimer_access, NULL, gt_phys_ctl_write, NULL, raw_write, NULL },
    { "CNTP_CTL(S)", 15,14,2, 0,0,1, 0, ARM_CP_IO | ARM_CP_ALIAS,
      PL1_RW | PL0_R, ARM_CP_SECSTATE_S, NULL, 0, offsetoflow32(CPUARMState, cp15.c14_timer[GTIMER_SEC].ctl), {0, 0},
      gt_ptimer_access, NULL, gt_sec_ctl_write, NULL, raw_write },
    { "CNTP_CTL_EL0", 0,14,2, 3,3,1, ARM_CP_STATE_AA64,
      ARM_CP_IO, PL1_RW | PL0_R, 0, NULL, 0, offsetof(CPUARMState, cp15.c14_timer[GTIMER_PHYS].ctl), {0, 0},
      gt_ptimer_access, NULL,gt_phys_ctl_write, NULL,raw_write, },
    { "CNTV_CTL", 15,14,3, 0,0,1, 0,
      ARM_CP_IO | ARM_CP_ALIAS, PL1_RW | PL0_R, 0, NULL, 0, offsetoflow32(CPUARMState, cp15.c14_timer[GTIMER_VIRT].ctl), {0, 0},
      gt_vtimer_access, NULL, gt_virt_ctl_write, NULL, raw_write, NULL },
    { "CNTV_CTL_EL0", 0,14,3, 3,3,1, ARM_CP_STATE_AA64,
      ARM_CP_IO, PL1_RW | PL0_R, 0, NULL, 0, offsetof(CPUARMState, cp15.c14_timer[GTIMER_VIRT].ctl), {0, 0},
      gt_vtimer_access, NULL,gt_virt_ctl_write, NULL,raw_write, },
    /* TimerValue views: a 32 bit downcounting view of the underlying state */
    { "CNTP_TVAL", 15,14,2, 0,0,0, 0,
      ARM_CP_NO_RAW | ARM_CP_IO, PL1_RW | PL0_R, ARM_CP_SECSTATE_NS, NULL, 0, 0, {0, 0},
      gt_ptimer_access, gt_phys_tval_read, gt_phys_tval_write, },
    { "CNTP_TVAL(S)", 15,14,2, 0,0,0, 0, ARM_CP_NO_RAW | ARM_CP_IO,
      PL1_RW | PL0_R, ARM_CP_SECSTATE_S, NULL, 0, 0, {0, 0},
      gt_ptimer_access, gt_sec_tval_read, gt_sec_tval_write },
    { "CNTP_TVAL_EL0", 0,14,2, 3,3,0, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW | ARM_CP_IO, PL1_RW | PL0_R, 0, NULL, 0, 0, {0, 0},
      gt_ptimer_access, gt_phys_tval_read, gt_phys_tval_write, NULL, NULL, gt_phys_timer_reset },
    { "CNTV_TVAL", 15,14,3, 0,0,0, 0,
      ARM_CP_NO_RAW | ARM_CP_IO, PL1_RW | PL0_R, 0, NULL, 0, 0, {0, 0},
      gt_vtimer_access, gt_virt_tval_read, gt_virt_tval_write, },
    { "CNTV_TVAL_EL0", 0,14,3, 3,3,0, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW | ARM_CP_IO, PL1_RW | PL0_R, 0, NULL, 0, 0, {0, 0},
      gt_vtimer_access, gt_virt_tval_read, gt_virt_tval_write, NULL, NULL, gt_virt_timer_reset },
    /* The counter itself */
    { "CNTPCT", 15,0,14, 0,0, 0, 0,
      ARM_CP_64BIT | ARM_CP_NO_RAW | ARM_CP_IO, PL0_R, 0, NULL, 0, 0, {0, 0},
      gt_pct_access, gt_cnt_read,NULL, NULL,NULL, arm_cp_reset_ignore, },
    { "CNTPCT_EL0", 0,14,0, 3,3,1, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW | ARM_CP_IO, PL0_R, 0, NULL, 0, 0, {0, 0},
      gt_pct_access, gt_cnt_read, NULL, NULL, NULL, NULL },
    { "CNTVCT", 15,0,14, 0,1,0, 0,
      ARM_CP_64BIT | ARM_CP_NO_RAW | ARM_CP_IO, PL0_R, 0, NULL, 0, 0, {0, 0},
      gt_vct_access, gt_virt_cnt_read, NULL, NULL, NULL, arm_cp_reset_ignore, },
    { "CNTVCT_EL0", 0,14,0, 3,3,2, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW | ARM_CP_IO, PL0_R, 0, NULL, 0, 0, {0, 0},
      gt_vct_access, gt_virt_cnt_read, NULL, NULL, NULL, NULL },
    /* Comparison value, indicating when the timer goes off */
    { "CNTP_CVAL", 15, 0,14, 0,2, 0, 0,
      ARM_CP_64BIT | ARM_CP_IO | ARM_CP_ALIAS, PL1_RW | PL0_R, ARM_CP_SECSTATE_NS, NULL, 0, offsetof(CPUARMState, cp15.c14_timer[GTIMER_PHYS].cval), {0, 0},
      gt_ptimer_access, NULL, gt_phys_cval_write, NULL, raw_write, NULL },
    { "CNTP_CVAL(S)", 15,0,14, 0,2,0, 0, ARM_CP_64BIT | ARM_CP_IO | ARM_CP_ALIAS,
      PL1_RW | PL0_R, ARM_CP_SECSTATE_S, NULL, 0, offsetof(CPUARMState, cp15.c14_timer[GTIMER_SEC].cval), {0, 0},
      gt_ptimer_access, NULL, gt_sec_cval_write, NULL, raw_write },
    { "CNTP_CVAL_EL0", 0,14,2, 3,3,2, ARM_CP_STATE_AA64,
      ARM_CP_IO, PL1_RW | PL0_R, 0, NULL, 0, offsetof(CPUARMState, cp15.c14_timer[GTIMER_PHYS].cval), {0, 0},
      gt_ptimer_access, NULL, gt_phys_cval_write, NULL, raw_write, },
    { "CNTV_CVAL", 15, 0,14, 0,3,0, 0,
      ARM_CP_64BIT | ARM_CP_IO | ARM_CP_ALIAS, PL1_RW | PL0_R, 0, NULL, 0, offsetof(CPUARMState, cp15.c14_timer[GTIMER_VIRT].cval), {0, 0},
      gt_vtimer_access, NULL, gt_virt_cval_write, NULL, raw_write, NULL },
    { "CNTV_CVAL_EL0", 0,14,3, 3,3,2, ARM_CP_STATE_AA64,
      ARM_CP_IO, PL1_RW | PL0_R, 0, NULL, 0, offsetof(CPUARMState, cp15.c14_timer[GTIMER_VIRT].cval), {0, 0},
      gt_vtimer_access, NULL, gt_virt_cval_write, NULL, raw_write, },
    /* Secure timer -- this is actually restricted to only EL3
     * and configurably Secure-EL1 via the accessfn.
     */
    { "CNTPS_TVAL_EL1", 0,14,2, 3,7,0, ARM_CP_STATE_AA64, ARM_CP_NO_RAW | ARM_CP_IO,
      PL1_RW, 0, NULL, 0, 0, {0, 0},
      gt_stimer_access, gt_sec_tval_read, gt_sec_tval_write,
      NULL, NULL, gt_sec_timer_reset },
    { "CNTPS_CTL_EL1", 0,14,2, 3,7,1, ARM_CP_STATE_AA64, ARM_CP_IO,
      PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c14_timer[GTIMER_SEC].ctl), {0, 0},
      gt_stimer_access, NULL, gt_sec_ctl_write, NULL, raw_write },
    { "CNTPS_CVAL_EL1", 0,14,2, 3,7,2, ARM_CP_STATE_AA64, ARM_CP_IO,
      PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c14_timer[GTIMER_SEC].cval), {0, 0},
      gt_stimer_access, NULL, gt_sec_cval_write, NULL, raw_write },
    REGINFO_SENTINEL
};

#else
/* In user-mode none of the generic timer registers are accessible,
 * and their implementation depends on QEMU_CLOCK_VIRTUAL and qdev gpio outputs,
 * so instead just don't register any of them.
 */
static const ARMCPRegInfo generic_timer_cp_reginfo[] = {
    REGINFO_SENTINEL
};

#endif

static void par_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    if (arm_feature(env, ARM_FEATURE_LPAE)) {
        raw_write(env, ri, value);
    } else if (arm_feature(env, ARM_FEATURE_V7)) {
        raw_write(env, ri, value & 0xfffff6ff);
    } else {
        raw_write(env, ri, value & 0xfffff1ff);
    }
}

#ifndef CONFIG_USER_ONLY
/* get_phys_addr() isn't present for user-mode-only targets */

static CPAccessResult ats_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                 bool isread)
{
    if (ri->opc2 & 4) {
        /* The ATS12NSO* operations must trap to EL3 if executed in
         * Secure EL1 (which can only happen if EL3 is AArch64).
         * They are simply UNDEF if executed from NS EL1.
         * They function normally from EL2 or EL3.
         */
        if (arm_current_el(env) == 1) {
            if (arm_is_secure_below_el3(env)) {
                return CP_ACCESS_TRAP_UNCATEGORIZED_EL3;
            }
            return CP_ACCESS_TRAP_UNCATEGORIZED;
        }
    }
    return CP_ACCESS_OK;
}

static uint64_t do_ats_write(CPUARMState *env, uint64_t value,
                             MMUAccessType access_type, ARMMMUIdx mmu_idx)
{
    hwaddr phys_addr;
    target_ulong page_size;
    int prot;
    bool ret;
    uint64_t par64;
    bool format64 = false;
    MemTxAttrs attrs = {0};
    ARMMMUFaultInfo fi = {0};
    ARMCacheAttrs cacheattrs = {0};

    ret = get_phys_addr(env, value, access_type, mmu_idx, &phys_addr, &attrs,
                        &prot, &page_size, &fi, &cacheattrs);

    if (is_a64(env)) {
        format64 = true;
    } else if (arm_feature(env, ARM_FEATURE_LPAE)) {
        /*
         * ATS1Cxx:
         * * TTBCR.EAE determines whether the result is returned using the
         *   32-bit or the 64-bit PAR format
         * * Instructions executed in Hyp mode always use the 64bit format
         *
         * ATS1S2NSOxx uses the 64bit format if any of the following is true:
         * * The Non-secure TTBCR.EAE bit is set to 1
         * * The implementation includes EL2, and the value of HCR.VM is 1
         *
         * ATS1Hx always uses the 64bit format (not supported yet).
         */
        format64 = arm_s1_regime_using_lpae_format(env, mmu_idx);

        if (arm_feature(env, ARM_FEATURE_EL2)) {
            if (mmu_idx == ARMMMUIdx_S12NSE0 || mmu_idx == ARMMMUIdx_S12NSE1) {
                format64 |= env->cp15.hcr_el2 & HCR_VM;
            } else {
                format64 |= arm_current_el(env) == 2;
            }
        }
    }

    if (format64) {
        /* Create a 64-bit PAR */
        par64 = (1 << 11); /* LPAE bit always set */
        if (!ret) {
            par64 |= phys_addr & ~0xfffULL;
            if (!attrs.secure) {
                par64 |= (1 << 9); /* NS */
            }
            par64 |= (uint64_t)cacheattrs.attrs << 56; /* ATTR */
            par64 |= cacheattrs.shareability << 7; /* SH */
        } else {
            uint32_t fsr = arm_fi_to_lfsc(&fi);

            par64 |= 1; /* F */
            par64 |= (fsr & 0x3f) << 1; /* FS */
            /* Note that S2WLK and FSTAGE are always zero, because we don't
             * implement virtualization and therefore there can't be a stage 2
             * fault.
             */
        }
    } else {
        /* fsr is a DFSR/IFSR value for the short descriptor
         * translation table format (with WnR always clear).
         * Convert it to a 32-bit PAR.
         */
        if (!ret) {
            /* We do not set any attribute bits in the PAR */
            if (page_size == (1 << 24)
                && arm_feature(env, ARM_FEATURE_V7)) {
                par64 = (phys_addr & 0xff000000) | (1 << 1);
            } else {
                par64 = phys_addr & 0xfffff000;
            }
            if (!attrs.secure) {
                par64 |= (1 << 9); /* NS */
            }
        } else {
            uint32_t fsr = arm_fi_to_sfsc(&fi);

            par64 = ((fsr & (1 << 10)) >> 5) | ((fsr & (1 << 12)) >> 6) |
                    ((fsr & 0xf) << 1) | 1;
        }
    }
    return par64;
}

static void ats_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    MMUAccessType access_type = ri->opc2 & 1 ? MMU_DATA_STORE : MMU_DATA_LOAD;
    uint64_t par64;
    ARMMMUIdx mmu_idx;
    int el = arm_current_el(env);
    bool secure = arm_is_secure_below_el3(env);

    switch (ri->opc2 & 6) {
    case 0:
        /* stage 1 current state PL1: ATS1CPR, ATS1CPW */
        switch (el) {
        case 3:
            mmu_idx = ARMMMUIdx_S1E3;
            break;
        case 2:
            mmu_idx = ARMMMUIdx_S1NSE1;
            break;
        case 1:
            mmu_idx = secure ? ARMMMUIdx_S1SE1 : ARMMMUIdx_S1NSE1;
            break;
        default:
            g_assert_not_reached();
        }
        break;
    case 2:
        /* stage 1 current state PL0: ATS1CUR, ATS1CUW */
        switch (el) {
        case 3:
            mmu_idx = ARMMMUIdx_S1SE0;
            break;
        case 2:
            mmu_idx = ARMMMUIdx_S1NSE0;
            break;
        case 1:
            mmu_idx = secure ? ARMMMUIdx_S1SE0 : ARMMMUIdx_S1NSE0;
            break;
        default:
            g_assert_not_reached();
        }
        break;
    case 4:
        /* stage 1+2 NonSecure PL1: ATS12NSOPR, ATS12NSOPW */
        mmu_idx = ARMMMUIdx_S12NSE1;
        break;
    case 6:
        /* stage 1+2 NonSecure PL0: ATS12NSOUR, ATS12NSOUW */
        mmu_idx = ARMMMUIdx_S12NSE0;
        break;
    default:
        g_assert_not_reached();
    }

    par64 = do_ats_write(env, value, access_type, mmu_idx);

    A32_BANKED_CURRENT_REG_SET(env, par, par64);
}

static void ats1h_write(CPUARMState *env, const ARMCPRegInfo *ri,
                        uint64_t value)
{
    MMUAccessType access_type = ri->opc2 & 1 ? MMU_DATA_STORE : MMU_DATA_LOAD;
    uint64_t par64;

    par64 = do_ats_write(env, value, access_type, ARMMMUIdx_S2NS);

    A32_BANKED_CURRENT_REG_SET(env, par, par64);
}

static CPAccessResult at_s1e2_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                     bool isread)
{
    if (arm_current_el(env) == 3 && !(env->cp15.scr_el3 & SCR_NS)) {
        return CP_ACCESS_TRAP;
    }
    return CP_ACCESS_OK;
}

static void ats_write64(CPUARMState *env, const ARMCPRegInfo *ri,
                        uint64_t value)
{
    MMUAccessType access_type = ri->opc2 & 1 ? MMU_DATA_STORE : MMU_DATA_LOAD;
    ARMMMUIdx mmu_idx;
    int secure = arm_is_secure_below_el3(env);

    switch (ri->opc2 & 6) {
    case 0:
        switch (ri->opc1) {
        case 0: /* AT S1E1R, AT S1E1W */
            mmu_idx = secure ? ARMMMUIdx_S1SE1 : ARMMMUIdx_S1NSE1;
            break;
        case 4: /* AT S1E2R, AT S1E2W */
            mmu_idx = ARMMMUIdx_S1E2;
            break;
        case 6: /* AT S1E3R, AT S1E3W */
            mmu_idx = ARMMMUIdx_S1E3;
            break;
        default:
            g_assert_not_reached();
        }
        break;
    case 2: /* AT S1E0R, AT S1E0W */
        mmu_idx = secure ? ARMMMUIdx_S1SE0 : ARMMMUIdx_S1NSE0;
        break;
    case 4: /* AT S12E1R, AT S12E1W */
        mmu_idx = secure ? ARMMMUIdx_S1SE1 : ARMMMUIdx_S12NSE1;
        break;
    case 6: /* AT S12E0R, AT S12E0W */
        mmu_idx = secure ? ARMMMUIdx_S1SE0 : ARMMMUIdx_S12NSE0;
        break;
    default:
        g_assert_not_reached();
    }

    env->cp15.par_el[1] = do_ats_write(env, value, access_type, mmu_idx);
}
#endif

static const ARMCPRegInfo vapa_cp_reginfo[] = {
    { "PAR", 15,7,4, 0,0,0, 0,
      0, PL1_RW, 0, NULL, 0, 0,
      { offsetoflow32(CPUARMState, cp15.par_s), offsetoflow32(CPUARMState, cp15.par_ns) },
      NULL, NULL, par_write },
#ifndef CONFIG_USER_ONLY
    /* This underdecoding is safe because the reginfo is NO_RAW. */
    { "ATS", 15,7,8, 0,0,CP_ANY, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      ats_access, NULL, ats_write },
#endif
    REGINFO_SENTINEL
};

/* Return basic MPU access permission bits.  */
static uint32_t simple_mpu_ap_bits(uint32_t val)
{
    uint32_t ret;
    uint32_t mask;
    int i;
    ret = 0;
    mask = 3;
    for (i = 0; i < 16; i += 2) {
        ret |= (val >> i) & mask;
        mask <<= 2;
    }
    return ret;
}

/* Pad basic MPU access permission bits to extended format.  */
static uint32_t extended_mpu_ap_bits(uint32_t val)
{
    uint32_t ret;
    uint32_t mask;
    int i;
    ret = 0;
    mask = 3;
    for (i = 0; i < 16; i += 2) {
        ret |= (val & mask) << i;
        mask <<= 2;
    }
    return ret;
}

static void pmsav5_data_ap_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                 uint64_t value)
{
    env->cp15.pmsav5_data_ap = extended_mpu_ap_bits(value);
}

static uint64_t pmsav5_data_ap_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return simple_mpu_ap_bits(env->cp15.pmsav5_data_ap);
}

static void pmsav5_insn_ap_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                 uint64_t value)
{
    env->cp15.pmsav5_insn_ap = extended_mpu_ap_bits(value);
}

static uint64_t pmsav5_insn_ap_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return simple_mpu_ap_bits(env->cp15.pmsav5_insn_ap);
}

static uint64_t pmsav7_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    uint32_t *u32p = *(uint32_t **)raw_ptr(env, ri);

    if (!u32p) {
        return 0;
    }

    u32p += env->pmsav7.rnr[M_REG_NS];
    return *u32p;
}

static void pmsav7_write(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    uint32_t *u32p = *(uint32_t **)raw_ptr(env, ri);

    if (!u32p) {
        return;
    }

    u32p += env->pmsav7.rnr[M_REG_NS];
    tlb_flush(CPU(cpu)); /* Mappings may have changed - purge! */
    *u32p = value;
}

static void pmsav7_rgnr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    uint32_t nrgs = cpu->pmsav7_dregion;

    if (value >= nrgs) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "PMSAv7 RGNR write >= # supported regions, %" PRIu32
                      " > %" PRIu32 "\n", (uint32_t)value, nrgs);
        return;
    }

    raw_write(env, ri, value);
}

static const ARMCPRegInfo pmsav7_cp_reginfo[] = {
    /* Reset for all these registers is handled in arm_cpu_reset(),
     * because the PMSAv7 is also used by M-profile CPUs, which do
     * not register cpregs but still need the state to be reset.
     */
    { "DRBAR", 15,6,1, 0,0,0, 0,ARM_CP_NO_RAW,
      PL1_RW, 0, NULL, 0, offsetof(CPUARMState, pmsav7.drbar), {0, 0},
      NULL, pmsav7_read, pmsav7_write, NULL, NULL, arm_cp_reset_ignore },
    { "DRSR", 15,6,1, 0,0,2, 0,ARM_CP_NO_RAW,
      PL1_RW, 0, NULL, 0, offsetof(CPUARMState, pmsav7.drsr), {0, 0},
      NULL, pmsav7_read, pmsav7_write, NULL, NULL, arm_cp_reset_ignore },
    { "DRACR", 15,6,1, 0,0,4, 0,ARM_CP_NO_RAW,
      PL1_RW, 0, NULL, 0, offsetof(CPUARMState, pmsav7.dracr), {0, 0},
      NULL, pmsav7_read, pmsav7_write, NULL, NULL, arm_cp_reset_ignore },
    { "RGNR", 15,6,2, 0,0,0, 0,0,
      PL1_RW, 0, NULL, 0, offsetof(CPUARMState, pmsav7.rnr[M_REG_NS]), {0, 0},
      NULL, NULL, pmsav7_rgnr_write, NULL, NULL, arm_cp_reset_ignore },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo pmsav5_cp_reginfo[] = {
    { "DATA_AP", 15,5,0, 0,0,0, 0,
      ARM_CP_ALIAS, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.pmsav5_data_ap), {0, 0},
      NULL, pmsav5_data_ap_read, pmsav5_data_ap_write, },
    { "INSN_AP", 15,5,0, 0,0,1, 0,
      ARM_CP_ALIAS,PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.pmsav5_insn_ap), {0, 0},
      NULL, pmsav5_insn_ap_read, pmsav5_insn_ap_write, },
    { "DATA_EXT_AP", 15,5,0, 0,0,2, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.pmsav5_data_ap), },
    { "INSN_EXT_AP", 15,5,0, 0,0,3, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.pmsav5_insn_ap), },
    { "DCACHE_CFG", 15,2,0, 0,0,0, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c2_data), },
    { "ICACHE_CFG", 15,2,0, 0,0,1, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c2_insn), },
    /* Protection region base and size registers */
    { "946_PRBS0", 15,6,0, 0,0,CP_ANY, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c6_region[0]) },
    { "946_PRBS1", 15,6,1, 0,0,CP_ANY, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c6_region[1]) },
    { "946_PRBS2", 15,6,2, 0,0,CP_ANY, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c6_region[2]) },
    { "946_PRBS3", 15,6,3, 0,0,CP_ANY, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c6_region[3]) },
    { "946_PRBS4", 15,6,4, 0,0,CP_ANY, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c6_region[4]) },
    { "946_PRBS5", 15,6,5, 0,0,CP_ANY, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c6_region[5]) },
    { "946_PRBS6", 15,6,6, 0,0,CP_ANY, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c6_region[6]) },
    { "946_PRBS7", 15,6,7, 0,0,CP_ANY, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c6_region[7]) },
    REGINFO_SENTINEL
};

static void vmsa_ttbcr_raw_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                 uint64_t value)
{
    TCR *tcr = raw_ptr(env, ri);
    int maskshift = extract32(value, 0, 3);

    if (!arm_feature(env, ARM_FEATURE_V8)) {
        if (arm_feature(env, ARM_FEATURE_LPAE) && (value & TTBCR_EAE)) {
            /* Pre ARMv8 bits [21:19], [15:14] and [6:3] are UNK/SBZP when
             * using Long-desciptor translation table format */
            value &= ~((7 << 19) | (3 << 14) | (0xf << 3));
        } else if (arm_feature(env, ARM_FEATURE_EL3)) {
            /* In an implementation that includes the Security Extensions
             * TTBCR has additional fields PD0 [4] and PD1 [5] for
             * Short-descriptor translation table format.
             */
            value &= TTBCR_PD1 | TTBCR_PD0 | TTBCR_N;
        } else {
            value &= TTBCR_N;
        }
    }

    /* Update the masks corresponding to the TCR bank being written
     * Note that we always calculate mask and base_mask, but
     * they are only used for short-descriptor tables (ie if EAE is 0);
     * for long-descriptor tables the TCR fields are used differently
     * and the mask and base_mask values are meaningless.
     */
    tcr->raw_tcr = value;
    tcr->mask = ~(((uint32_t)0xffffffffu) >> maskshift);
    tcr->base_mask = ~((uint32_t)0x3fffu >> maskshift);
}

static void vmsa_ttbcr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                             uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    if (arm_feature(env, ARM_FEATURE_LPAE)) {
        /* With LPAE the TTBCR could result in a change of ASID
         * via the TTBCR.A1 bit, so do a TLB flush.
         */
        tlb_flush(CPU(cpu));
    }
    vmsa_ttbcr_raw_write(env, ri, value);
}

static void vmsa_ttbcr_reset(CPUARMState *env, const ARMCPRegInfo *ri)
{
    TCR *tcr = raw_ptr(env, ri);

    /* Reset both the TCR as well as the masks corresponding to the bank of
     * the TCR being reset.
     */
    tcr->raw_tcr = 0;
    tcr->mask = 0;
    tcr->base_mask = 0xffffc000u;
}

static void vmsa_tcr_el1_write(CPUARMState *env, const ARMCPRegInfo *ri,
                               uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    TCR *tcr = raw_ptr(env, ri);

    /* For AArch64 the A1 bit could result in a change of ASID, so TLB flush. */
    tlb_flush(CPU(cpu));
    tcr->raw_tcr = value;
}

static void vmsa_ttbr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    /* 64 bit accesses to the TTBRs can change the ASID and so we
     * must flush the TLB.
     */
    if (cpreg_field_is_64bit(ri)) {
        ARMCPU *cpu = arm_env_get_cpu(env);

        tlb_flush(CPU(cpu));
    }
    raw_write(env, ri, value);
}

static void vttbr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                        uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    CPUState *cs = CPU(cpu);

    /* Accesses to VTTBR may change the VMID so we must flush the TLB.  */
    if (raw_read(env, ri) != value) {
        tlb_flush_by_mmuidx(cs,
                            ARMMMUIdxBit_S12NSE1 |
                            ARMMMUIdxBit_S12NSE0 |
                            ARMMMUIdxBit_S2NS);
        raw_write(env, ri, value);
    }
}

static const ARMCPRegInfo vmsa_pmsa_cp_reginfo[] = {
    { "DFSR", 15,5,0, 0,0,0, 0,
      ARM_CP_ALIAS, PL1_RW, 0, NULL, 0, 0,
      { offsetoflow32(CPUARMState, cp15.dfsr_s), offsetoflow32(CPUARMState, cp15.dfsr_ns) },
      NULL, NULL, NULL, NULL, NULL, NULL },
    { "IFSR", 15,5,0, 0,0,1, 0,
      0, PL1_RW, 0, NULL, 0, 0,
      { offsetoflow32(CPUARMState, cp15.ifsr_s), offsetoflow32(CPUARMState, cp15.ifsr_ns) }},
    { "FAR_EL1", 0,6,0, 3,0,0, ARM_CP_STATE_AA64,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.far_el[1]), },
    { "DFAR", 15,6,0, 0,0,0, 0,0,
      PL1_RW, 0, NULL, 0, 0,
      { offsetof(CPUARMState, cp15.dfar_s), offsetof(CPUARMState, cp15.dfar_ns) } },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo vmsa_cp_reginfo[] = {
    { "ESR_EL1", 0,5,2, 3,0,0, ARM_CP_STATE_AA64,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.esr_el[1]), },
    { "TTBR0_EL1", 0,2,0, 3,0,0, ARM_CP_STATE_BOTH,
      0, PL1_RW, 0, NULL, 0, 0,
      { offsetof(CPUARMState, cp15.ttbr0_s), offsetof(CPUARMState, cp15.ttbr0_ns) },
      NULL, NULL, vmsa_ttbr_write, },
    { "TTBR1_EL1", 0,2,0, 3,0,1, ARM_CP_STATE_BOTH,
      0, PL1_RW, 0, NULL, 0, 0,
      { offsetof(CPUARMState, cp15.ttbr1_s), offsetof(CPUARMState, cp15.ttbr1_ns) },
      NULL, NULL, vmsa_ttbr_write, },
    { "TCR_EL1", 0,2,0, 3,0,2, ARM_CP_STATE_AA64,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.tcr_el[1]), {0, 0},
      NULL, NULL,vmsa_tcr_el1_write, NULL,raw_write, vmsa_ttbcr_reset, },
    { "TTBCR", 15,2,0, 0,0,2, 0,
      ARM_CP_ALIAS, PL1_RW, 0, NULL, 0, 0,
      { offsetoflow32(CPUARMState, cp15.tcr_el[3]), offsetoflow32(CPUARMState, cp15.tcr_el[1]) },
      NULL, NULL, vmsa_ttbcr_write, NULL, vmsa_ttbcr_raw_write, NULL },
    REGINFO_SENTINEL
};

static void omap_ticonfig_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                uint64_t value)
{
    env->cp15.c15_ticonfig = value & 0xe7;
    /* The OS_TYPE bit in this register changes the reported CPUID! */
    env->cp15.c0_cpuid = (value & (1 << 5)) ?
        ARM_CPUID_TI915T : ARM_CPUID_TI925T;
}

static void omap_threadid_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                uint64_t value)
{
    env->cp15.c15_threadid = value & 0xffff;
}

static void omap_wfi_write(CPUARMState *env, const ARMCPRegInfo *ri,
                           uint64_t value)
{
    /* Wait-for-interrupt (deprecated) */
    cpu_interrupt(CPU(arm_env_get_cpu(env)), CPU_INTERRUPT_HALT);
}

static void omap_cachemaint_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                  uint64_t value)
{
    /* On OMAP there are registers indicating the max/min index of dcache lines
     * containing a dirty line; cache flush operations have to reset these.
     */
    env->cp15.c15_i_max = 0x000;
    env->cp15.c15_i_min = 0xff0;
}

static const ARMCPRegInfo omap_cp_reginfo[] = {
    { "DFSR", 15,5,CP_ANY, 0,CP_ANY,CP_ANY, 0,
      ARM_CP_OVERRIDE, PL1_RW, 0, NULL, 0, offsetoflow32(CPUARMState, cp15.esr_el[1]), },
    { "", 15,15,0, 0,0,0, 0,
      ARM_CP_NOP, PL1_RW, 0, NULL, 0, 0, },
    { "TICONFIG", 15,15,1, 0,0,0, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c15_ticonfig), {0, 0},
      NULL, NULL, omap_ticonfig_write },
    { "IMAX", 15,15,2, 0,0,0, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c15_i_max), },
    { "IMIN", 15,15,3, 0,0,0, 0,
      0, PL1_RW, 0, NULL, 0xff0, offsetof(CPUARMState, cp15.c15_i_min) },
    { "THREADID", 15,15,4, 0,0,0, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c15_threadid), {0, 0},
      NULL, NULL, omap_threadid_write },
    { "TI925T_STATUS", 15,15,8, 0,0,0, 0,
      ARM_CP_NO_RAW, PL1_RW, 0, NULL, 0, 0, {0, 0},
      NULL, arm_cp_read_zero, omap_wfi_write, },
    /* TODO: Peripheral port remap register:
     * On OMAP2 mcr p15, 0, rn, c15, c2, 4 sets up the interrupt controller
     * base address at $rn & ~0xfff and map size of 0x200 << ($rn & 0xfff),
     * when MMU is off.
     */
    { "OMAP_CACHEMAINT", 15,7,CP_ANY, 0,0,CP_ANY, 0,
      ARM_CP_OVERRIDE | ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, omap_cachemaint_write },
    { "C9", 15,9,CP_ANY, 0,CP_ANY,CP_ANY, 0,
      ARM_CP_CONST | ARM_CP_OVERRIDE, PL1_RW, 0, NULL, 0, 0, },
    REGINFO_SENTINEL
};

static void xscale_cpar_write(CPUARMState *env, const ARMCPRegInfo *ri,
                              uint64_t value)
{
    env->cp15.c15_cpar = value & 0x3fff;
}

static const ARMCPRegInfo xscale_cp_reginfo[] = {
    { "XSCALE_CPAR", 15,15,1, 0,0,0, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c15_cpar), {0, 0},
      NULL, NULL, xscale_cpar_write, },
    { "XSCALE_AUXCR", 15,1,0, 0,0,1, 0,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c1_xscaleauxcr), },
    /* XScale specific cache-lockdown: since we have no cache we NOP these
     * and hope the guest does not really rely on cache behaviour.
     */
    { "XSCALE_LOCK_ICACHE_LINE", 15,9,1, 0,0,0, 0,
      ARM_CP_NOP, PL1_W },
    { "XSCALE_UNLOCK_ICACHE", 15,9,1, 0,0,1, 0,
      ARM_CP_NOP, PL1_W, },
    { "XSCALE_DCACHE_LOCK", 15,9,2, 0,0,0, 0,
      ARM_CP_NOP, PL1_RW },
    { "XSCALE_UNLOCK_DCACHE", 15,9,2, 0,0,1, 0,
      ARM_CP_NOP, PL1_W, },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo dummy_c15_cp_reginfo[] = {
    /* RAZ/WI the whole crn=15 space, when we don't have a more specific
     * implementation of this implementation-defined space.
     * Ideally this should eventually disappear in favour of actually
     * implementing the correct behaviour for all cores.
     */
    { "C15_IMPDEF", 15,15,CP_ANY, 0,CP_ANY,CP_ANY, 0,
      ARM_CP_CONST | ARM_CP_NO_RAW | ARM_CP_OVERRIDE, PL1_RW, 0, NULL, 0 },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo cache_dirty_status_cp_reginfo[] = {
    /* Cache status: RAZ because we have no cache so it's always clean */
    { "CDSR", 15,7,10, 0,0,6, 0,
      ARM_CP_CONST | ARM_CP_NO_RAW, PL1_R, 0, NULL, 0 },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo cache_block_ops_cp_reginfo[] = {
    /* We never have a a block transfer operation in progress */
    { "BXSR", 15,7,12, 0,0,4, 0,
      ARM_CP_CONST | ARM_CP_NO_RAW, PL0_R, 0, NULL, 0 },
    /* The cache ops themselves: these all NOP for QEMU */
    { "IICR", 15, 0,5, 0,0, 0, 0,
      ARM_CP_NOP|ARM_CP_64BIT, PL1_W },
    { "IDCR", 15, 0,6, 0,0, 0, 0,
      ARM_CP_NOP|ARM_CP_64BIT, PL1_W, },
    { "CDCR", 15, 0,12, 0,0, 0, 0,
      ARM_CP_NOP|ARM_CP_64BIT, PL0_W, },
    { "PIR", 15, 0,12, 0,1, 0, 0,
      ARM_CP_NOP|ARM_CP_64BIT, PL0_W, },
    { "PDR", 15, 0,12, 0,2, 0, 0,
      ARM_CP_NOP|ARM_CP_64BIT, PL0_W, },
    { "CIDCR", 15, 0,14, 0,0, 0, 0,
      ARM_CP_NOP|ARM_CP_64BIT, PL1_W, },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo cache_test_clean_cp_reginfo[] = {
    /* The cache test-and-clean instructions always return (1 << 30)
     * to indicate that there are no dirty cache lines.
     */
    { "TC_DCACHE", 15,7,10, 0,0,3, 0,
      ARM_CP_CONST | ARM_CP_NO_RAW, PL0_R, 0, NULL, (1 << 30) },
    { "TCI_DCACHE", 15,7,14, 0,0,3, 0,
      ARM_CP_CONST | ARM_CP_NO_RAW, PL0_R, 0, NULL, (1 << 30) },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo strongarm_cp_reginfo[] = {
    /* Ignore ReadBuffer accesses */
    { "C9_READBUFFER", 15,9,CP_ANY, 0,CP_ANY,CP_ANY, 0,
      ARM_CP_CONST | ARM_CP_OVERRIDE | ARM_CP_NO_RAW, PL1_RW, 0, NULL, 0, },
    REGINFO_SENTINEL
};

static uint64_t midr_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    unsigned int cur_el = arm_current_el(env);
    bool secure = arm_is_secure(env);

    if (arm_feature(&cpu->env, ARM_FEATURE_EL2) && !secure && cur_el == 1) {
        return env->cp15.vpidr_el2;
    }
    return raw_read(env, ri);
}

static uint64_t mpidr_read_val(CPUARMState *env)
{
    ARMCPU *cpu = ARM_CPU(env->uc, arm_env_get_cpu(env));
    uint64_t mpidr = cpu->mp_affinity;

    if (arm_feature(env, ARM_FEATURE_V7MP)) {
        mpidr |= (1U << 31);
        /* Cores which are uniprocessor (non-coherent)
         * but still implement the MP extensions set
         * bit 30. (For instance, Cortex-R5).
         */
        if (cpu->mp_is_up) {
            mpidr |= (1u << 30);
        }
    }
    return mpidr;
}

static uint64_t mpidr_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    unsigned int cur_el = arm_current_el(env);
    bool secure = arm_is_secure(env);

    if (arm_feature(env, ARM_FEATURE_EL2) && !secure && cur_el == 1) {
        return env->cp15.vmpidr_el2;
    }
    return mpidr_read_val(env);
}

static const ARMCPRegInfo mpidr_cp_reginfo[] = {
    { "MPIDR", 0,0,0, 3,0,5, ARM_CP_STATE_BOTH,
      ARM_CP_NO_RAW, PL1_R, 0, NULL, 0, 0, {0, 0},
      NULL, mpidr_read, },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo lpae_cp_reginfo[] = {
    /* NOP AMAIR0/1 */
    { "AMAIR0", 0,10,3, 3,0,0, ARM_CP_STATE_BOTH,
      ARM_CP_CONST, PL1_RW, 0, NULL, 0 },
    /* AMAIR1 is mapped to AMAIR_EL1[63:32] */
    { "AMAIR1", 15,10,3, 0,0,1, 0,
      ARM_CP_CONST, PL1_RW, 0, NULL, 0 },
    { "PAR", 15, 0,7, 0,0, 0, 0,
      ARM_CP_64BIT, PL1_RW, 0, NULL, 0, 0,
      { offsetof(CPUARMState, cp15.par_s), offsetof(CPUARMState, cp15.par_ns) } },
    { "TTBR0", 15, 0,2, 0,0, 0, 0,
      ARM_CP_64BIT | ARM_CP_ALIAS, PL1_RW, 0, NULL, 0, 0,
      { offsetof(CPUARMState, cp15.ttbr0_s), offsetof(CPUARMState, cp15.ttbr0_ns) },
      NULL, NULL, vmsa_ttbr_write, NULL, NULL, NULL },
    { "TTBR1", 15, 0,2, 0,1, 0, 0,
      ARM_CP_64BIT | ARM_CP_ALIAS, PL1_RW, 0, NULL, 0, 0,
      { offsetof(CPUARMState, cp15.ttbr1_s), offsetof(CPUARMState, cp15.ttbr1_ns) },
      NULL, NULL, vmsa_ttbr_write, NULL, NULL, NULL },
    REGINFO_SENTINEL
};

static uint64_t aa64_fpcr_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return vfp_get_fpcr(env);
}

static void aa64_fpcr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    vfp_set_fpcr(env, value);
}

static uint64_t aa64_fpsr_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return vfp_get_fpsr(env);
}

static void aa64_fpsr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    vfp_set_fpsr(env, value);
}

static CPAccessResult aa64_daif_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                       bool isread)
{
    if (arm_current_el(env) == 0 && !(env->cp15.sctlr_el[1] & SCTLR_UMA)) {
        return CP_ACCESS_TRAP;
    }
    return CP_ACCESS_OK;
}

static void aa64_daif_write(CPUARMState *env, const ARMCPRegInfo *ri,
                            uint64_t value)
{
    env->daif = value & PSTATE_DAIF;
}

static CPAccessResult aa64_cacheop_access(CPUARMState *env,
                                          const ARMCPRegInfo *ri,
                                          bool isread)
{
    /* Cache invalidate/clean: NOP, but EL0 must UNDEF unless
     * SCTLR_EL1.UCI is set.
     */
    if (arm_current_el(env) == 0 && !(env->cp15.sctlr_el[1] & SCTLR_UCI)) {
        return CP_ACCESS_TRAP;
    }
    return CP_ACCESS_OK;
}

/* See: D4.7.2 TLB maintenance requirements and the TLB maintenance instructions
 * Page D4-1736 (DDI0487A.b)
 */

static void tlbi_aa64_vmalle1_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                    uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    CPUState *cs = CPU(cpu);

    if (arm_is_secure_below_el3(env)) {
        tlb_flush_by_mmuidx(cs,
                            ARMMMUIdxBit_S1SE1 |
                            ARMMMUIdxBit_S1SE0);
    } else {
        tlb_flush_by_mmuidx(cs,
                            ARMMMUIdxBit_S12NSE1 |
                            ARMMMUIdxBit_S12NSE0);
    }
}

static void tlbi_aa64_vmalle1is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                uint64_t value)
{
// UNICORN: TODO: issue #642
#if 0
    bool sec = arm_is_secure_below_el3(env);
    CPUState *cs = ENV_GET_CPU(env);

    if (sec) {
        tlb_flush_by_mmuidx_all_cpus_synced(cs,
                                            ARMMMUIdxBit_S1SE1 |
                                            ARMMMUIdxBit_S1SE0);
    } else {
        tlb_flush_by_mmuidx_all_cpus_synced(cs,
                                            ARMMMUIdxBit_S12NSE1 |
                                            ARMMMUIdxBit_S12NSE0);
    }
#endif
}

static void tlbi_aa64_alle1_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                  uint64_t value)
{
    /* Note that the 'ALL' scope must invalidate both stage 1 and
     * stage 2 translations, whereas most other scopes only invalidate
     * stage 1 translations.
     */
    ARMCPU *cpu = arm_env_get_cpu(env);
    CPUState *cs = CPU(cpu);

    if (arm_is_secure_below_el3(env)) {
        tlb_flush_by_mmuidx(cs,
                            ARMMMUIdxBit_S1SE1 |
                            ARMMMUIdxBit_S1SE0);
    } else {
        if (arm_feature(env, ARM_FEATURE_EL2)) {
            tlb_flush_by_mmuidx(cs,
                                ARMMMUIdxBit_S12NSE1 |
                                ARMMMUIdxBit_S12NSE0 |
                                ARMMMUIdxBit_S2NS);
        } else {
            tlb_flush_by_mmuidx(cs,
                                ARMMMUIdxBit_S12NSE1 |
                                ARMMMUIdxBit_S12NSE0);
        }
    }
}

static void tlbi_aa64_alle2_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                  uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    CPUState *cs = CPU(cpu);

    tlb_flush_by_mmuidx(cs, ARMMMUIdxBit_S1E2);
}

static void tlbi_aa64_alle3_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                  uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    CPUState *cs = CPU(cpu);

    tlb_flush_by_mmuidx(cs, ARMMMUIdxBit_S1E3);
}

static void tlbi_aa64_alle1is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                    uint64_t value)
{
    /* Note that the 'ALL' scope must invalidate both stage 1 and
     * stage 2 translations, whereas most other scopes only invalidate
     * stage 1 translations.
     */
// UNICORN: TODO: issue #642
#if 0
    bool sec = arm_is_secure_below_el3(env);
    bool has_el2 = arm_feature(env, ARM_FEATURE_EL2);
    CPUState *cs = ENV_GET_CPU(env);

    if (sec) {
        tlb_flush_by_mmuidx_all_cpus_synced(cs,
                                            ARMMMUIdxBit_S1SE1 |
                                            ARMMMUIdxBit_S1SE0);
    } else if (has_el2) {
        tlb_flush_by_mmuidx_all_cpus_synced(cs,
                                            ARMMMUIdxBit_S12NSE1 |
                                            ARMMMUIdxBit_S12NSE0 |
                                            ARMMMUIdxBit_S2NS);
    } else {
          tlb_flush_by_mmuidx_all_cpus_synced(cs,
                                              ARMMMUIdxBit_S12NSE1 |
                                              ARMMMUIdxBit_S12NSE0);
    }
#endif
}

static void tlbi_aa64_alle2is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                    uint64_t value)
{
// UNICORN: TODO: issue #642
#if 0
    CPUState *cs = ENV_GET_CPU(env);

    tlb_flush_by_mmuidx_all_cpus_synced(cs, ARMMMUIdxBit_S1E2);
#endif
}

static void tlbi_aa64_alle3is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                    uint64_t value)
{
// UNICORN: TODO: issue #642
#if 0
    CPUState *cs = ENV_GET_CPU(env);

    tlb_flush_by_mmuidx_all_cpus_synced(cs, ARMMMUIdxBit_S1E3);
#endif
}

static void tlbi_aa64_vae1_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                 uint64_t value)
{
    /* Invalidate by VA, EL1&0 (AArch64 version).
     * Currently handles all of VAE1, VAAE1, VAALE1 and VALE1,
     * since we don't support flush-for-specific-ASID-only or
     * flush-last-level-only.
     */
    ARMCPU *cpu = arm_env_get_cpu(env);
    CPUState *cs = CPU(cpu);
    uint64_t pageaddr = sextract64(value << 12, 0, 56);

    if (arm_is_secure_below_el3(env)) {
        tlb_flush_page_by_mmuidx(cs, pageaddr,
                                 ARMMMUIdxBit_S1SE1 |
                                 ARMMMUIdxBit_S1SE0);
    } else {
        tlb_flush_page_by_mmuidx(cs, pageaddr,
                                 ARMMMUIdxBit_S12NSE1 |
                                 ARMMMUIdxBit_S12NSE0);
    }
}

static void tlbi_aa64_vae2_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                 uint64_t value)
{
    /* Invalidate by VA, EL2
     * Currently handles both VAE2 and VALE2, since we don't support
     * flush-last-level-only.
     */
    ARMCPU *cpu = arm_env_get_cpu(env);
    CPUState *cs = CPU(cpu);
    uint64_t pageaddr = sextract64(value << 12, 0, 56);

    tlb_flush_page_by_mmuidx(cs, pageaddr, ARMMMUIdxBit_S1E2);
}

static void tlbi_aa64_vae3_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                 uint64_t value)
{
    /* Invalidate by VA, EL3
     * Currently handles both VAE3 and VALE3, since we don't support
     * flush-last-level-only.
     */
    ARMCPU *cpu = arm_env_get_cpu(env);
    CPUState *cs = CPU(cpu);
    uint64_t pageaddr = sextract64(value << 12, 0, 56);

    tlb_flush_page_by_mmuidx(cs, pageaddr, ARMMMUIdxBit_S1E3);
}

static void tlbi_aa64_vae1is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                   uint64_t value)
{
// UNICORN: TODO: issue #642
#if 0
    bool sec = arm_is_secure_below_el3(env);
    CPUState *cs = ENV_GET_CPU(env)
    uint64_t pageaddr = sextract64(value << 12, 0, 56);

    if (sec) {
        tlb_flush_page_by_mmuidx_all_cpus_synced(cs, pageaddr,
                                                 ARMMMUIdxBit_S1SE1 |
                                                 ARMMMUIdxBit_S1SE0);
    } else {
        tlb_flush_page_by_mmuidx_all_cpus_synced(cs, pageaddr,
                                                 ARMMMUIdxBit_S12NSE1 |
                                                 ARMMMUIdxBit_S12NSE0);
    }
#endif
}

static void tlbi_aa64_vae2is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                   uint64_t value)
{
// UNICORN: TODO: issue #642
#if 0
    CPUState *cs = ENV_GET_CPU(env);
    uint64_t pageaddr = sextract64(value << 12, 0, 56);

    tlb_flush_page_by_mmuidx_all_cpus_synced(cs, pageaddr,
                                             ARMMMUIdxBit_S1E2);
#endif
}

static void tlbi_aa64_vae3is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                   uint64_t value)
{
// UNICORN: TODO: issue #642
#if 0
    CPUState *cs = ENV_GET_CPU(env);
    uint64_t pageaddr = sextract64(value << 12, 0, 56);

    tlb_flush_page_by_mmuidx_all_cpus_synced(cs, pageaddr,
                                             ARMMMUIdxBit_S1E3);
#endif
}

static void tlbi_aa64_ipas2e1_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                    uint64_t value)
{
    /* Invalidate by IPA. This has to invalidate any structures that
     * contain only stage 2 translation information, but does not need
     * to apply to structures that contain combined stage 1 and stage 2
     * translation information.
     * This must NOP if EL2 isn't implemented or SCR_EL3.NS is zero.
     */
    ARMCPU *cpu = arm_env_get_cpu(env);
    CPUState *cs = CPU(cpu);
    uint64_t pageaddr;

    if (!arm_feature(env, ARM_FEATURE_EL2) || !(env->cp15.scr_el3 & SCR_NS)) {
        return;
    }

    pageaddr = sextract64(value << 12, 0, 48);

    tlb_flush_page_by_mmuidx(cs, pageaddr, ARMMMUIdxBit_S2NS);
}

static void tlbi_aa64_ipas2e1is_write(CPUARMState *env, const ARMCPRegInfo *ri,
                                      uint64_t value)
{
// UNICORN: TODO: issue #642
#if 0
    CPUState *cs = ENV_GET_CPU(env);
    uint64_t pageaddr;

    if (!arm_feature(env, ARM_FEATURE_EL2) || !(env->cp15.scr_el3 & SCR_NS)) {
        return;
    }

    pageaddr = sextract64(value << 12, 0, 48);

    tlb_flush_page_by_mmuidx_all_cpus_synced(cs, pageaddr,
                                             ARMMMUIdxBit_S2NS);
#endif
}

static CPAccessResult aa64_zva_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                      bool isread)
{
    /* We don't implement EL2, so the only control on DC ZVA is the
     * bit in the SCTLR which can prohibit access for EL0.
     */
    if (arm_current_el(env) == 0 && !(env->cp15.sctlr_el[1] & SCTLR_DZE)) {
        return CP_ACCESS_TRAP;
    }
    return CP_ACCESS_OK;
}

static uint64_t aa64_dczid_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    int dzp_bit = 1 << 4;

    /* DZP indicates whether DC ZVA access is allowed */
    if (aa64_zva_access(env, NULL, false) == CP_ACCESS_OK) {
        dzp_bit = 0;
    }
    return cpu->dcz_blocksize | dzp_bit;
}

static CPAccessResult sp_el0_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                    bool isread)
{
    if (!(env->pstate & PSTATE_SP)) {
        /* Access to SP_EL0 is undefined if it's being used as
         * the stack pointer.
         */
        return CP_ACCESS_TRAP_UNCATEGORIZED;
    }
    return CP_ACCESS_OK;
}

static uint64_t spsel_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    return env->pstate & PSTATE_SP;
}

static void spsel_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t val)
{
    update_spsel(env, val);
}

static void sctlr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                        uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    if (raw_read(env, ri) == value) {
        /* Skip the TLB flush if nothing actually changed; Linux likes
         * to do a lot of pointless SCTLR writes.
         */
        return;
    }

    if (arm_feature(env, ARM_FEATURE_PMSA) && !cpu->has_mpu) {
        /* M bit is RAZ/WI for PMSA with no MPU implemented */
        value &= ~SCTLR_M;
    }

    raw_write(env, ri, value);
    /* ??? Lots of these bits are not implemented.  */
    /* This may enable/disable the MMU, so do a TLB flush.  */
    tlb_flush(CPU(cpu));
}

static CPAccessResult fpexc32_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                     bool isread)
{
    if ((env->cp15.cptr_el[2] & CPTR_TFP) && arm_current_el(env) == 2) {
        return CP_ACCESS_TRAP_FP_EL2;
    }
    if (env->cp15.cptr_el[3] & CPTR_TFP) {
        return CP_ACCESS_TRAP_FP_EL3;
    }
    return CP_ACCESS_OK;
}

static void sdcr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                       uint64_t value)
{
    env->cp15.mdcr_el3 = value & SDCR_VALID_MASK;
}

static const ARMCPRegInfo v8_cp_reginfo[] = {
    /* Minimal set of EL0-visible registers. This will need to be expanded
     * significantly for system emulation of AArch64 CPUs.
     */
    { "NZCV", 0,4,2, 3,3,0, ARM_CP_STATE_AA64,
      ARM_CP_NZCV, PL0_RW,  },
    { "DAIF", 0,4,2, 3,3,1, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL0_RW, 0, NULL, 0, offsetof(CPUARMState, daif), {0, 0},
      aa64_daif_access, NULL, aa64_daif_write, NULL,NULL, arm_cp_reset_ignore },
    { "FPCR", 0,4,4, 3,3,0, ARM_CP_STATE_AA64, ARM_CP_FPU | ARM_CP_SUPPRESS_TB_END,
      PL0_RW, 0, NULL, 0, 0, {0, 0},
      NULL, aa64_fpcr_read, aa64_fpcr_write },
    { "FPSR", 0,4,4, 3,3,1, ARM_CP_STATE_AA64, ARM_CP_FPU | ARM_CP_SUPPRESS_TB_END,
      PL0_RW, 0, NULL, 0, 0, {0, 0},
      NULL, aa64_fpsr_read, aa64_fpsr_write },
    { "DCZID_EL0", 0,0,0, 3,3,7, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL0_R, 0, NULL, 0, 0, {0, 0},
      NULL, aa64_dczid_read },
    { "DC_ZVA", 0,7,4, 1,3,1, ARM_CP_STATE_AA64,
      ARM_CP_DC_ZVA, PL0_W, 0, NULL, 0, 0, {0, 0},
#ifndef CONFIG_USER_ONLY
      /* Avoid overhead of an access check that always passes in user-mode */
      aa64_zva_access,
#endif
    },
    { "CURRENTEL", 0,4,2, 3,0,2, ARM_CP_STATE_AA64,
      ARM_CP_CURRENTEL, PL1_R, },
    /* Cache ops: all NOPs since we don't emulate caches */
    { "IC_IALLUIS", 0,7,1, 1,0,0, ARM_CP_STATE_AA64,
      ARM_CP_NOP, PL1_W, },
    { "IC_IALLU", 0,7,5, 1,0,0, ARM_CP_STATE_AA64,
      ARM_CP_NOP, PL1_W, },
    { "IC_IVAU", 0,7,5, 1,3,1, ARM_CP_STATE_AA64,
      ARM_CP_NOP, PL0_W, 0, NULL, 0, 0, {0, 0},
      aa64_cacheop_access },
    { "DC_IVAC", 0,7,6, 1,0,1, ARM_CP_STATE_AA64,
      ARM_CP_NOP, PL1_W, },
    { "DC_ISW", 0,7,6, 1,0,2, ARM_CP_STATE_AA64,
      ARM_CP_NOP, PL1_W, },
    { "DC_CVAC", 0,7,10, 1,3,1, ARM_CP_STATE_AA64,
      ARM_CP_NOP, PL0_W, 0, NULL, 0, 0, {0, 0},
      aa64_cacheop_access },
    { "DC_CSW", 0,7,10, 1,0,2, ARM_CP_STATE_AA64,
      ARM_CP_NOP, PL1_W, },
    { "DC_CVAU", 0,7,11, 1,3,1, ARM_CP_STATE_AA64,
      ARM_CP_NOP, PL0_W, 0, NULL, 0, 0, {0, 0},
      aa64_cacheop_access },
    { "DC_CIVAC", 0,7,14, 1,3,1, ARM_CP_STATE_AA64,
      ARM_CP_NOP, PL0_W, 0, NULL, 0, 0, {0, 0},
      aa64_cacheop_access },
    { "DC_CISW", 0,7,14, 1,0,2, ARM_CP_STATE_AA64,
      ARM_CP_NOP, PL1_W,  },
    /* TLBI operations */
    { "TLBI_VMALLE1IS", 0,8,3, 1,0,0, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vmalle1is_write },
    { "TLBI_VAE1IS", 0,8,3, 1,0,1, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae1is_write },
    { "TLBI_ASIDE1IS", 0,8,3, 1,0,2, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vmalle1is_write },
    { "TLBI_VAAE1IS", 0,8,3, 1,0,3, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae1is_write },
    { "TLBI_VALE1IS", 0,8,3, 1,0,5, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae1is_write },
    { "TLBI_VAALE1IS", 0,8,3, 1,0,7, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae1is_write },
    { "TLBI_VMALLE1", 0,8,7, 1,0,0, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vmalle1_write },
    { "TLBI_VAE1", 0,8,7, 1,0,1, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae1_write },
    { "TLBI_ASIDE1", 0,8,7, 1,0,2, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vmalle1_write },
    { "TLBI_VAAE1", 0,8,7, 1,0,3, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae1_write },
    { "TLBI_VALE1", 0,8,7, 1,0,5, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae1_write },
    { "TLBI_VAALE1", 0,8,7, 1,0,7, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae1_write },
    { "TLBI_VMALLS12E1IS", 0,8,3, 1,4,6, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_alle1is_write },
    { "TLBI_IPAS2E1IS", 0,8,0, 1,4,1, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_ipas2e1is_write },
    { "TLBI_IPAS2LE1IS", 0,8,0, 1,4,5, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_ipas2e1is_write },
    { "TLBI_ALLE1IS", 0,8,3, 1,4,4, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_alle1is_write },
    { "TLBI_IPAS2E1", 0,8,4, 1,4,1, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_ipas2e1_write },
    { "TLBI_IPAS2LE1", 0,8,4, 1,4,5, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_ipas2e1_write },
    { "TLBI_ALLE1", 0,8,7, 1,4,4, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_alle1_write },
    { "TLBI_VMALLS12E1", 0,8,7, 1,4,6, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_alle1is_write },
#ifndef CONFIG_USER_ONLY
    /* 64 bit address translation operations */
    { "AT_S1E1R", 0,7,8, 1,0,0, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, ats_write64 },
    { "AT_S1E1W", 0,7,8, 1,0,1, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, ats_write64 },
    { "AT_S1E0R", 0,7,8, 1,0,2, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, ats_write64 },
    { "AT_S1E0W", 0,7,8, 1,0,3, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, ats_write64 },
    { "AT_S12E1R", 0,7,8, 1,4,4, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, ats_write64 },
    { "AT_S12E1W", 0,7,8, 1,4,5, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, ats_write64 },
    { "AT_S12E0R", 0,7,8, 1,4,6, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, ats_write64 },
    { "AT_S12E0W", 0,7,8, 1,4,7, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, ats_write64 },
    /* AT S1E2* are elsewhere as they UNDEF from EL3 if EL2 is not present */
    { "AT_S1E3R", 0,7,8, 1,6,0, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL3_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, ats_write64 },
    { "AT_S1E3W", 0,7,8, 1,6,1, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL3_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, ats_write64 },
    { "PAR_EL1", 0,7,4, 3,0,0, ARM_CP_STATE_AA64, ARM_CP_ALIAS,
      PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.par_el[1]), {0, 0},
      NULL, NULL, par_write },
#endif
    /* TLB invalidate last level of translation table walk */
    { "TLBIMVALIS", 15,8,3, 0,0,5, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimva_is_write },
    { "TLBIMVAALIS", 15,8,3, 0,0,7, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimvaa_is_write },
    { "TLBIMVAL", 15,8,7, 0,0,5, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimva_write },
    { "TLBIMVAAL", 15,8,7, 0,0,7, 0,
      ARM_CP_NO_RAW, PL1_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimvaa_write },
    { "TLBIMVALH", 15,8,7, 0,4,5, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimva_hyp_write },
    { "TLBIMVALHIS", 15,8,3, 0,4,5, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimva_hyp_is_write },
    { "TLBIIPAS2", 15,8,4, 0,4,1, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiipas2_write },
    { "TLBIIPAS2IS", 15,8,0, 0,4,1, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiipas2_is_write },
    { "TLBIIPAS2L", 15,8,4, 0,4,5, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiipas2_write },
    { "TLBIIPAS2LIS", 15,8,0, 0,4,5, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiipas2_is_write },
    /* 32 bit cache operations */
    { "ICIALLUIS", 15,7,1, 0,0,0, 0,
      ARM_CP_NOP, PL1_W },
    { "BPIALLUIS", 15,7,1, 0,0,6, 0,
      ARM_CP_NOP, PL1_W },
    { "ICIALLU", 15,7,5, 0,0,0, 0,
      ARM_CP_NOP, PL1_W },
    { "ICIMVAU", 15,7,5, 0,0,1, 0,
      ARM_CP_NOP, PL1_W },
    { "BPIALL", 15,7,5, 0,0,6, 0,
      ARM_CP_NOP, PL1_W },
    { "BPIMVA", 15,7,5, 0,0,7, 0,
      ARM_CP_NOP, PL1_W },
    { "DCIMVAC", 15,7,6, 0,0,1, 0,
      ARM_CP_NOP, PL1_W },
    { "DCISW", 15,7,6, 0,0,2, 0,
      ARM_CP_NOP, PL1_W },
    { "DCCMVAC", 15,7,10, 0,0,1, 0,
      ARM_CP_NOP, PL1_W },
    { "DCCSW", 15,7,10, 0,0,2, 0,
      ARM_CP_NOP, PL1_W },
    { "DCCMVAU", 15,7,11, 0,0,1, 0,
      ARM_CP_NOP, PL1_W },
    { "DCCIMVAC", 15,7,14, 0,0,1, 0,
      ARM_CP_NOP, PL1_W },
    { "DCCISW", 15,7,14, 0,0,2, 0,
      ARM_CP_NOP, PL1_W },
    /* MMU Domain access control / MPU write buffer control */
    { "DACR", 15,3,0, 0,0,0, 0,
      0, PL1_RW, 0, NULL, 0, 0,
      { offsetoflow32(CPUARMState, cp15.dacr_s), offsetoflow32(CPUARMState, cp15.dacr_ns) },
      NULL, NULL,dacr_write, NULL,raw_write, },
    { "ELR_EL1", 0,4,0, 3,0,1, ARM_CP_STATE_AA64,
      ARM_CP_ALIAS, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, elr_el[1]) },
    { "SPSR_EL1", 0,4,0, 3,0,0, ARM_CP_STATE_AA64,
      ARM_CP_ALIAS, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, banked_spsr[BANK_SVC]) },
    /* We rely on the access checks not allowing the guest to write to the
     * state field when SPSel indicates that it's being used as the stack
     * pointer.
     */
    { "SP_EL0", 0,4,1, 3,0,0, ARM_CP_STATE_AA64,
      ARM_CP_ALIAS, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, sp_el[0]), {0, 0},
      sp_el0_access, },
    { "SP_EL1", 0,4,1, 3,4,0, ARM_CP_STATE_AA64, ARM_CP_ALIAS,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, sp_el[1]) },
    { "SPSel", 0,4,2, 3,0,0, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL1_RW, 0, NULL, 0, 0, {0, 0},
      NULL, spsel_read, spsel_write },
    { "FPEXC32_EL2", 0,5,3, 3,4,0, ARM_CP_STATE_AA64, ARM_CP_ALIAS,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, vfp.xregs[ARM_VFP_FPEXC]), {0, 0},
      fpexc32_access },
    { "DACR32_EL2", 0,3,0, 3,4,0, ARM_CP_STATE_AA64,0,
       PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.dacr32_el2), {0, 0},
       NULL, NULL, dacr_write, NULL, raw_write },
    { "IFSR32_EL2", 0,5,0, 3,4,1, ARM_CP_STATE_AA64,0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.ifsr32_el2) },
    { "SPSR_IRQ", 0,4,3, 3,4,0, ARM_CP_STATE_AA64, ARM_CP_ALIAS,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, banked_spsr[BANK_IRQ]) },
    { "SPSR_ABT", 0,4,3, 3,4,1, ARM_CP_STATE_AA64, ARM_CP_ALIAS,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, banked_spsr[BANK_ABT]) },
    { "SPSR_UND", 0,4,3, 3,4,2, ARM_CP_STATE_AA64, ARM_CP_ALIAS,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, banked_spsr[BANK_UND]) },
    { "SPSR_FIQ", 0,4,3, 3,4,3, ARM_CP_STATE_AA64, ARM_CP_ALIAS,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, banked_spsr[BANK_FIQ]) },
    { "MDCR_EL3", 0,1,3, 3,6,1, ARM_CP_STATE_AA64, 0,
      PL3_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.mdcr_el3) },
    { "SDCR", 15,1,3, 0,0,1, 0, ARM_CP_ALIAS,
      PL1_RW, 0, NULL, 0, offsetoflow32(CPUARMState, cp15.mdcr_el3), {0, 0},
      access_trap_aa32s_el1, NULL, sdcr_write },
    REGINFO_SENTINEL
};

/* Used to describe the behaviour of EL2 regs when EL2 does not exist.  */
static const ARMCPRegInfo el3_no_el2_cp_reginfo[] = {
    { "VBAR_EL2", 0,12,0, 3,4,0, ARM_CP_STATE_AA64,
      0, PL2_RW, 0, NULL, 0, 0, {0, 0},
      NULL, arm_cp_read_zero, arm_cp_write_ignore },
    { "HCR_EL2", 0,1,1, 3,4,0, ARM_CP_STATE_AA64,
      ARM_CP_NO_RAW, PL2_RW, 0, NULL, 0, 0, {0, 0},
      NULL, arm_cp_read_zero, arm_cp_write_ignore },
    { "CPTR_EL2", 0,1,1, 3,4,2, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "MAIR_EL2", 0,10,2, 3,4,0, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "HMAIR1", 0,10,2, 0,4,1, ARM_CP_STATE_AA32, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "AMAIR_EL2", 0,10,3, 3,4,0, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "HMAIR1", 0,10,3, 0,4,1, ARM_CP_STATE_AA32, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "AFSR0_EL2", 0,5,1, 3,4,0, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "AFSR1_EL2", 0,5,1, 3,4,1, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "TCR_EL2", 0,2,0, 3,4,2, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "VTCR_EL2", 0,2,1, 3,4,2, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0, 0, {0, 0},
      access_el3_aa32ns_aa64any },
    { "VTTBR", 15,0,2, 0,6,0, ARM_CP_STATE_AA32, ARM_CP_CONST | ARM_CP_64BIT,
      PL2_RW, 0, NULL, 0, 0, {0, 0},
      access_el3_aa32ns },
    { "VTTBR_EL2", 0,2,1, 3,4,0, ARM_CP_STATE_AA64, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "SCTLR_EL2", 0,1,0, 3,4,0, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "TPIDR_EL2", 0,13,0, 3,4,2, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "TTBR0_EL2", 0,2,0, 3,4,0, ARM_CP_STATE_AA64, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "HTTBR", 15,0,2, 0,4,0, 0, ARM_CP_64BIT | ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "CNTHCTL_EL2", 0,14,1, 3,4,0, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "CNTVOFF_EL2", 0,14,0, 3,4,3, ARM_CP_STATE_AA64, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "CNTVOFF", 15,0,14, 0,4,0, ARM_CP_64BIT | ARM_CP_CONST, 0,
      PL2_RW, 0, NULL, 0 },
    { "CNTHP_CVAL_EL2", 0,14,2, 3,4,2, ARM_CP_STATE_AA64, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "CNTHP_CVAL", 15,0,14, 0,6,0, 0, ARM_CP_64BIT | ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "CNTHP_TVAL_EL2", 0,14,2, 3,4,0, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "CNTHP_CTL_EL2", 0,14,2, 3,4,1, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "MDCR_EL2", 0,1,1, 3,4,1, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0, 0, {0, 0},
      access_tda },
    { "HPFAR_EL2", 0,6,0, 3,4,4, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0, 0, {0, 0},
      access_el3_aa32ns_aa64any },
    { "HSTR_EL2", 0,1,1, 3,4,3, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    REGINFO_SENTINEL
};

static void hcr_write(CPUARMState *env, const ARMCPRegInfo *ri, uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    uint64_t valid_mask = HCR_MASK;

    if (arm_feature(env, ARM_FEATURE_EL3)) {
        valid_mask &= ~HCR_HCD;
    } else if (cpu->psci_conduit != QEMU_PSCI_CONDUIT_SMC) {
        /* Architecturally HCR.TSC is RES0 if EL3 is not implemented.
         * However, if we're using the SMC PSCI conduit then QEMU is
         * effectively acting like EL3 firmware and so the guest at
         * EL2 should retain the ability to prevent EL1 from being
         * able to make SMC calls into the ersatz firmware, so in
         * that case HCR.TSC should be read/write.
         */
        valid_mask &= ~HCR_TSC;
    }

    /* Clear RES0 bits.  */
    value &= valid_mask;

    /* These bits change the MMU setup:
     * HCR_VM enables stage 2 translation
     * HCR_PTW forbids certain page-table setups
     * HCR_DC Disables stage1 and enables stage2 translation
     */
    if ((raw_read(env, ri) ^ value) & (HCR_VM | HCR_PTW | HCR_DC)) {
        tlb_flush(CPU(cpu));
    }
    raw_write(env, ri, value);
}

static const ARMCPRegInfo el2_cp_reginfo[] = {
    { "HCR_EL2", 0,1,1, 3,4,0, ARM_CP_STATE_AA64,
      0, PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.hcr_el2), {0, 0},
      NULL, NULL, hcr_write },
    { "ELR_EL2", 0,4,0, 3,4,1, ARM_CP_STATE_AA64,
      ARM_CP_ALIAS, PL2_RW, 0, NULL, 0, offsetof(CPUARMState, elr_el[2]) },
    { "ESR_EL2", 0,5,2, 3,4,0, ARM_CP_STATE_AA64, 0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.esr_el[2]) },
    { "FAR_EL2", 0,6,0, 3,4,0, ARM_CP_STATE_AA64,
      0, PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.far_el[2]) },
    { "SPSR_EL2", 0,4,0, 3,4,0, ARM_CP_STATE_AA64,
      ARM_CP_ALIAS, PL2_RW, 0, NULL, 0, offsetof(CPUARMState, banked_spsr[BANK_HYP]) },
    { "VBAR_EL2", 0,12,0, 3,4,0, ARM_CP_STATE_AA64,
      0, PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.vbar_el[2]), {0, 0},
      NULL, NULL, vbar_write, },
    { "SP_EL2", 0,4,1, 3,6,0, ARM_CP_STATE_AA64, ARM_CP_ALIAS,
      PL3_RW, 0, NULL, 0, offsetof(CPUARMState, sp_el[2]) },
    { "CPTR_EL2", 0,1,1, 3,4,2, ARM_CP_STATE_BOTH, 0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.cptr_el[2]), {0, 0},
      cptr_access },
    { "MAIR_EL2", 0,10,2, 3,4,0, ARM_CP_STATE_BOTH, 0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.mair_el[2]) },
    { "HMAIR1", 0,10,2, 0,4,1, ARM_CP_STATE_AA32, ARM_CP_ALIAS,
      PL2_RW, 0, NULL, 0, offsetofhigh32(CPUARMState, cp15.mair_el[2]) },
    { "AMAIR_EL2", 0,10,3, 3,4,0, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    /* HAMAIR1 is mapped to AMAIR_EL2[63:32] */
    { "HMAIR1", 0,10,3, 0,4,1, ARM_CP_STATE_AA32, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "AFSR0_EL2", 0,5,1, 3,4,0, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "AFSR1_EL2", 0,5,1, 3,4,1, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL2_RW, 0, NULL, 0 },
    { "TCR_EL2", 0,2,0, 3,4,2, ARM_CP_STATE_BOTH, 0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.tcr_el[2]), {0, 0},
      /* no .writefn needed as this can't cause an ASID change;
       * no .raw_writefn or .resetfn needed as we never use mask/base_mask
       */
      NULL, NULL, NULL, NULL, NULL, NULL },
    { "VTCR", 15,2,1, 0,4,2, ARM_CP_STATE_AA32, ARM_CP_ALIAS,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.vtcr_el2), {0, 0},
      access_el3_aa32ns },
    { "VTCR_EL2", 0,2,1, 3,4,2, ARM_CP_STATE_AA64, 0,
      PL2_RW, 0, NULL, 0,
      /* no .writefn needed as this can't cause an ASID change;
       * no .raw_writefn or .resetfn needed as we never use mask/base_mask
       */
      offsetof(CPUARMState, cp15.vtcr_el2) },
    { "VTTBR", 15,0,2, 0,6,0, ARM_CP_STATE_AA32, ARM_CP_64BIT | ARM_CP_ALIAS,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.vttbr_el2), {0, 0},
      access_el3_aa32ns, NULL, vttbr_write },
    { "VTTBR_EL2", 0,2,1, 3,4,0, ARM_CP_STATE_AA64, 0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.vttbr_el2), {0, 0},
      NULL, NULL, vttbr_write },
    { "SCTLR_EL2", 0,1,0, 3,4,0, ARM_CP_STATE_BOTH, 0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.sctlr_el[2]), {0, 0},
      NULL, NULL, sctlr_write, NULL, raw_write },
    { "TPIDR_EL2", 0,13,0, 3,4,2, ARM_CP_STATE_BOTH, 0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.tpidr_el[2]) },
    { "TTBR0_EL2", 0,2,0, 3,4,0, ARM_CP_STATE_AA64, 0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.ttbr0_el[2]) },
    { "HTTBR", 15,0,2, 0,4,0, 0, ARM_CP_64BIT | ARM_CP_ALIAS,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.ttbr0_el[2]) },
    { "TLBIALLNSNH", 15,8,7, 0,4,4, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiall_nsnh_write },
    { "TLBIALLNSNHIS", 15,8,3, 0,4,4, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiall_nsnh_is_write },
    { "TLBIALLH", 15,8,7, 0,4,0, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiall_hyp_write },
    { "TLBIALLHIS", 15,8,3, 0,4,0, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbiall_hyp_is_write },
    { "TLBIMVAH", 15,8,7, 0,4,1, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimva_hyp_write },
    { "TLBIMVAHIS", 15,8,3, 0,4,1, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbimva_hyp_is_write },
    { "TLBI_ALLE2", 0,8,7, 1,4,0, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_alle2_write },
    { "TLBI_VAE2", 0,8,7, 1,4,1, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae2_write },
    { "TLBI_VALE2", 0,8,7, 1,4,5, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae2_write },
    { "TLBI_ALLE2IS", 0,8,3, 1,4,0, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_alle2is_write },
    { "TLBI_VAE2IS", 0,8,3, 1,4,1, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae2is_write },
    { "TLBI_VALE2IS", 0,8,3, 1,4,5, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae2is_write },
#ifndef CONFIG_USER_ONLY
    /* Unlike the other EL2-related AT operations, these must
     * UNDEF from EL3 if EL2 is not implemented, which is why we
     * define them here rather than with the rest of the AT ops.
     */
    { "AT_S1E2R", 0,7,8, 1,4,0, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      at_s1e2_access, NULL, ats_write64 },
    { "AT_S1E2W", 0,7,8, 1,4,1, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      at_s1e2_access, NULL, ats_write64 },
    /* The AArch32 ATS1H* operations are CONSTRAINED UNPREDICTABLE
     * if EL2 is not implemented; we choose to UNDEF. Behaviour at EL3
     * with SCR.NS == 0 outside Monitor mode is UNPREDICTABLE; we choose
     * to behave as if SCR.NS was 1.
     */
    { "ATS1HR", 15,7,8, 0,4,0, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, ats1h_write },
    { "ATS1HW", 15,7,8, 0,4,1, 0, ARM_CP_NO_RAW,
      PL2_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, ats1h_write },
      /* ARMv7 requires bit 0 and 1 to reset to 1. ARMv8 defines the
       * reset values as IMPDEF. We choose to reset to 3 to comply with
       * both ARMv7 and ARMv8.
       */
    { "CNTHCTL_EL2", 0,14,1, 3,4,0, ARM_CP_STATE_BOTH, 0,
      PL2_RW, 0, NULL, 3, offsetof(CPUARMState, cp15.cnthctl_el2) },
    { "CNTVOFF_EL2", 0,140,0, 3,4,3, ARM_CP_STATE_AA64, ARM_CP_IO,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.cntvoff_el2), {0, 0},
      NULL, NULL, gt_cntvoff_write },
    { "CNTVOFF", 15,0,14, 0,4,0, 0, ARM_CP_64BIT | ARM_CP_ALIAS | ARM_CP_IO,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.cntvoff_el2), {0, 0},
      NULL, NULL, gt_cntvoff_write },
    { "CNTHP_CVAL_EL2", 0,14,2, 3,4,2, ARM_CP_STATE_AA64, ARM_CP_IO,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c14_timer[GTIMER_HYP].cval), {0, 0},
      NULL, NULL, gt_hyp_cval_write, NULL, raw_write },
    { "CNTHP_CVAL", 15,0,14, 0,6,0, 0, ARM_CP_64BIT | ARM_CP_IO,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c14_timer[GTIMER_HYP].cval), {0, 0},
      NULL, NULL, gt_hyp_cval_write, NULL, raw_write },
    { "CNTHP_TVAL_EL2", 0,14,2, 3,4,0, ARM_CP_STATE_BOTH, ARM_CP_NO_RAW | ARM_CP_IO,
      PL2_RW, 0, NULL, 0, 0, {0, 0},
      NULL, gt_hyp_tval_read, gt_hyp_tval_write, NULL, NULL, gt_hyp_timer_reset },
    { "CNTHP_CTL_EL2", 0,14,2, 3,4,1, ARM_CP_STATE_BOTH, ARM_CP_IO,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.c14_timer[GTIMER_HYP].ctl), {0, 0},
      NULL, NULL, gt_hyp_ctl_write, NULL, raw_write },
#endif
    /* The only field of MDCR_EL2 that has a defined architectural reset value
     * is MDCR_EL2.HPMN which should reset to the value of PMCR_EL0.N; but we
     * don't impelment any PMU event counters, so using zero as a reset
     * value for MDCR_EL2 is okay
     */
    { "MDCR_EL2", 0,1,1, 3,4,1, ARM_CP_STATE_BOTH, 0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.mdcr_el2), },
    { "HPFAR", 15,6,0, 0,4,4, ARM_CP_STATE_AA32, 0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.hpfar_el2), {0, 0}, 
      access_el3_aa32ns },
    { "HPFAR_EL2", 0,6,0, 3,4,4, ARM_CP_STATE_AA64, 0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.hpfar_el2) },
    { "HSTR_EL2", 15,1,1, 3,4,3, ARM_CP_STATE_BOTH, 0,
      PL2_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.hstr_el2) },
    REGINFO_SENTINEL
};

static CPAccessResult nsacr_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                   bool isread)
{
    /* The NSACR is RW at EL3, and RO for NS EL1 and NS EL2.
     * At Secure EL1 it traps to EL3.
     */
    if (arm_current_el(env) == 3) {
        return CP_ACCESS_OK;
    }
    if (arm_is_secure_below_el3(env)) {
        return CP_ACCESS_TRAP_EL3;
    }
    /* Accesses from EL1 NS and EL2 NS are UNDEF for write but allow reads. */
    if (isread) {
        return CP_ACCESS_OK;
    }
    return CP_ACCESS_TRAP_UNCATEGORIZED;
}

static const ARMCPRegInfo el3_cp_reginfo[] = {
    { "SCR_EL3", 0,1,1, 3,6,0, ARM_CP_STATE_AA64,0,
      PL3_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.scr_el3), {0, 0},
      NULL, NULL, scr_write },
    { "SCR", 15,1,1, 0,0,0, 0,ARM_CP_ALIAS,
      PL1_RW, 0, NULL, 0, offsetoflow32(CPUARMState, cp15.scr_el3), {0, 0},
      access_trap_aa32s_el1, NULL, scr_write, NULL, NULL, NULL },
    { "SDER32_EL3", 0,1,1, 3,6,1, ARM_CP_STATE_AA64,0,
      PL3_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.sder) },
    { "SDER", 15,1,1, 0,0,1, 0,0,
      PL3_RW, 0, NULL, 0, offsetoflow32(CPUARMState, cp15.sder) },
    { "MVBAR", 15,12,0, 0,0,1, 0,0,
      PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.mvbar), {0, 0},
      access_trap_aa32s_el1, NULL, vbar_write },
    { "TTBR0_EL3", 0,2,0, 3,6,0, ARM_CP_STATE_AA64,0,
      PL3_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.ttbr0_el[3]), {0, 0},
      NULL, NULL, vmsa_ttbr_write },
    { "TCR_EL3", 0,2,0, 3,6,2, ARM_CP_STATE_AA64,0,
      PL3_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.tcr_el[3]), {0, 0},
      /* no .writefn needed as this can't cause an ASID change;
       * we must provide a .raw_writefn and .resetfn because we handle
       * reset and migration for the AArch32 TTBCR(S), which might be
       * using mask and base_mask.
       */
      NULL, NULL, NULL, NULL, vmsa_ttbcr_raw_write, vmsa_ttbcr_reset },
    { "ELR_EL3", 0,4,0, 3,6,1, ARM_CP_STATE_AA64,
      ARM_CP_ALIAS, PL3_RW, 0, NULL, 0, offsetof(CPUARMState, elr_el[3]) },
    { "ESR_EL3", 0,5,2, 3,6,0, ARM_CP_STATE_AA64, 0,
      PL3_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.esr_el[3]) },
    { "FAR_EL3", 0,6,0, 3,6,0, ARM_CP_STATE_AA64,
      0, PL3_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.far_el[3]) },
    { "SPSR_EL3", 0,4,0, 3,6,0, ARM_CP_STATE_AA64,
      ARM_CP_ALIAS, PL3_RW, 0, NULL, 0, offsetof(CPUARMState, banked_spsr[BANK_MON]) },
    { "VBAR_EL3", 0,12,0, 3,6,0, ARM_CP_STATE_AA64,
      0, PL3_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.vbar_el[3]), {0, 0},
      NULL, NULL, vbar_write, },
    { "CPTR_EL3", 0,1,1, 3,6,2, ARM_CP_STATE_AA64, 0,
      PL3_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.cptr_el[3]), {0, 0},
      cptr_access },
    { "TPIDR_EL3", 0,13,0, 3,6,2, ARM_CP_STATE_AA64, 0,
      PL3_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.tpidr_el[3]) },
    { "AMAIR_EL3", 0,10,3, 3,6,0, ARM_CP_STATE_AA64, ARM_CP_CONST,
      PL3_RW, 0, NULL, 0 },
    { "AFSR0_EL3", 0,5,1, 3,6,0, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL3_RW, 0, NULL, 0 },
    { "AFSR1_EL3", 0,5,1, 3,6,1, ARM_CP_STATE_BOTH, ARM_CP_CONST,
      PL3_RW, 0, NULL, 0 },
    { "TLBI_ALLE3IS", 0,8,3, 1,6,0, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL3_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_alle3is_write },
    { "TLBI_VAE3IS", 0,8,3, 1,6,1, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL3_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae3is_write },
    { "TLBI_VALE3IS", 0,8,3, 1,6,5, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL3_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae3is_write },
    { "TLBI_ALLE3", 0,8,7, 1,6,0, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL3_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_alle3_write },
    { "TLBI_VAE3", 0,8,7, 1,6,1, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL3_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae3_write },
    { "TLBI_VALE3", 0,8,7, 1,6,5, ARM_CP_STATE_AA64, ARM_CP_NO_RAW,
      PL3_W, 0, NULL, 0, 0, {0, 0},
      NULL, NULL, tlbi_aa64_vae3_write },
    REGINFO_SENTINEL
};

static CPAccessResult ctr_el0_access(CPUARMState *env, const ARMCPRegInfo *ri,
                                     bool isread)
{
    /* Only accessible in EL0 if SCTLR.UCT is set (and only in AArch64,
     * but the AArch32 CTR has its own reginfo struct)
     */
    if (arm_current_el(env) == 0 && !(env->cp15.sctlr_el[1] & SCTLR_UCT)) {
        return CP_ACCESS_TRAP;
    }
    return CP_ACCESS_OK;
}

static void oslar_write(CPUARMState *env, const ARMCPRegInfo *ri,
                        uint64_t value)
{
    /* Writes to OSLAR_EL1 may update the OS lock status, which can be
     * read via a bit in OSLSR_EL1.
     */
    int oslock;

    if (ri->state == ARM_CP_STATE_AA32) {
        oslock = (value == 0xC5ACCE55);
    } else {
        oslock = value & 1;
    }

    env->cp15.oslsr_el1 = deposit32(env->cp15.oslsr_el1, 1, 1, oslock);
}

static const ARMCPRegInfo debug_cp_reginfo[] = {
    /* DBGDRAR, DBGDSAR: always RAZ since we don't implement memory mapped
     * debug components. The AArch64 version of DBGDRAR is named MDRAR_EL1;
     * unlike DBGDRAR it is never accessible from EL0.
     * DBGDSAR is deprecated and must RAZ from v8 anyway, so it has no AArch64
     * accessor.
     */
    { "DBGDRAR", 14,1,0, 0,0,0, 0,
      ARM_CP_CONST, PL0_R, 0, NULL, 0, 0, {0, 0},
      access_tdra },
    { "MDRAR_EL1", 0,1,0, 2,0,0, ARM_CP_STATE_AA64,
      ARM_CP_CONST, PL1_R, 0, NULL, 0, 0, {0, 0},
      access_tdra },
    { "DBGDSAR", 14,2,0, 0,0,0, 0,
      ARM_CP_CONST, PL0_R, 0, NULL, 0, 0, {0, 0},
      access_tdra },
    /* Monitor debug system control register; the 32-bit alias is DBGDSCRext. */
    { "MDSCR_EL1", 14,0,2, 2,0,2, ARM_CP_STATE_BOTH,
      0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.mdscr_el1), },
    /* MDCCSR_EL0, aka DBGDSCRint. This is a read-only mirror of MDSCR_EL1.
     * We don't implement the configurable EL0 access.
     */
    { "MDCCSR_EL0", 14,0,1, 2,0,0, ARM_CP_STATE_BOTH,
      ARM_CP_ALIAS, PL1_R, 0, NULL, 0, offsetof(CPUARMState, cp15.mdscr_el1), {0, 0},
      access_tda, NULL, NULL, NULL, NULL, NULL },
    { "OSLAR_EL1", 14,1,0, 2,0,4, ARM_CP_STATE_BOTH, ARM_CP_NO_RAW,
      PL1_W, 0, NULL, 0, 0, {0, 0},
      access_tdosa, NULL, oslar_write },
    { "OSLSR_EL1", 14,1,1, 2,0,4, ARM_CP_STATE_BOTH, 0,
      PL1_R, 0, NULL, 10, offsetof(CPUARMState, cp15.oslsr_el1), {0, 0},
      access_tdosa },
    /* Dummy OSDLR_EL1: 32-bit Linux will read this */
    { "OSDLR_EL1", 14,1,3, 2,0,4, ARM_CP_STATE_BOTH,
      ARM_CP_NOP, PL1_RW, 0, NULL, 0, 0, {0, 0},
      access_tdosa },
    /* Dummy DBGVCR: Linux wants to clear this on startup, but we don't
     * implement vector catch debug events yet.
     */
    { "DBGVCR", 14,0,7, 0,0,0, 0,
      ARM_CP_NOP, PL1_RW, 0, NULL, 0, 0, {0, 0},
      access_tda },
    { "DBGVCR32_EL2", 0,0,7, 2,4,0, ARM_CP_STATE_AA64, ARM_CP_NOP,
      PL2_RW, 0, NULL, 0, 0, {0, 0},
      access_tda },
    /* Dummy MDCCINT_EL1, since we don't implement the Debug Communications
     * Channel but Linux may try to access this register. The 32-bit
     * alias is DBGDCCINT.
     */
    { "MDCCINT_EL1", 14,0,2, 2,0,0, ARM_CP_STATE_BOTH, ARM_CP_NOP,
      PL1_RW, 0, NULL, 0, 0, {0, 0},
      access_tda },
    REGINFO_SENTINEL
};

static const ARMCPRegInfo debug_lpae_cp_reginfo[] = {
    /* 64 bit access versions of the (dummy) debug registers */
    { "DBGDRAR", 14, 0,1, 0,0, 0, 0,
      ARM_CP_CONST|ARM_CP_64BIT, PL0_R, 0, NULL, 0 },
    { "DBGDSAR", 14, 0,2, 0,0, 0, 0,
      ARM_CP_CONST|ARM_CP_64BIT, PL0_R, 0, NULL, 0 },
    REGINFO_SENTINEL
};

/* Return the exception level to which SVE-disabled exceptions should
 * be taken, or 0 if SVE is enabled.
 */
static int sve_exception_el(CPUARMState *env)
{
#ifndef CONFIG_USER_ONLY
    unsigned current_el = arm_current_el(env);

    /* The CPACR.ZEN controls traps to EL1:
     * 0, 2 : trap EL0 and EL1 accesses
     * 1    : trap only EL0 accesses
     * 3    : trap no accesses
     */
    switch (extract32(env->cp15.cpacr_el1, 16, 2)) {
    default:
        if (current_el <= 1) {
            /* Trap to PL1, which might be EL1 or EL3 */
            if (arm_is_secure(env) && !arm_el_is_aa64(env, 3)) {
                return 3;
            }
            return 1;
        }
        break;
    case 1:
        if (current_el == 0) {
            return 1;
        }
        break;
    case 3:
        break;
    }

    /* Similarly for CPACR.FPEN, after having checked ZEN.  */
    switch (extract32(env->cp15.cpacr_el1, 20, 2)) {
    default:
        if (current_el <= 1) {
            if (arm_is_secure(env) && !arm_el_is_aa64(env, 3)) {
                return 3;
            }
            return 1;
        }
        break;
    case 1:
        if (current_el == 0) {
            return 1;
        }
        break;
    case 3:
        break;
    }

    /* CPTR_EL2.  Check both TZ and TFP.  */
    if (current_el <= 2
        && (env->cp15.cptr_el[2] & (CPTR_TFP | CPTR_TZ))
        && !arm_is_secure_below_el3(env)) {
        return 2;
    }

    /* CPTR_EL3.  Check both EZ and TFP.  */
    if (!(env->cp15.cptr_el[3] & CPTR_EZ)
        || (env->cp15.cptr_el[3] & CPTR_TFP)) {
        return 3;
    }
#endif
    return 0;
}

static void zcr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                      uint64_t value)
{
    /* Bits other than [3:0] are RAZ/WI.  */
    raw_write(env, ri, value & 0xf);
}

static const ARMCPRegInfo zcr_el1_reginfo = {
    "ZCR_EL1", 0,1,2, 3,0,0, ARM_CP_STATE_AA64, ARM_CP_SVE | ARM_CP_FPU,
    PL1_RW, 0, NULL, 0, offsetof(CPUARMState, vfp.zcr_el[1]), {0, 0},
    NULL, NULL, zcr_write, NULL, raw_write
};

static const ARMCPRegInfo zcr_el2_reginfo = {
    "ZCR_EL2", 0,1,2, 3,4,0, ARM_CP_STATE_AA64, ARM_CP_SVE | ARM_CP_FPU,
    PL2_RW, 0, NULL, 0, offsetof(CPUARMState, vfp.zcr_el[2]), {0, 0},
    NULL, NULL, zcr_write, NULL, raw_write
};

static const ARMCPRegInfo zcr_no_el2_reginfo = {
    "ZCR_EL2", 0,1,2, 3,4,0, ARM_CP_STATE_AA64, ARM_CP_SVE | ARM_CP_FPU,
    PL2_RW, 0, NULL, 0, 0, {0, 0},
    NULL, arm_cp_read_zero, arm_cp_write_ignore
};

static const ARMCPRegInfo zcr_el3_reginfo = {
    "ZCR_EL3", 0,1,2, 3,6,0, ARM_CP_STATE_AA64, ARM_CP_SVE | ARM_CP_FPU,
    PL3_RW, 0, NULL, 0, offsetof(CPUARMState, vfp.zcr_el[3]), {0, 0},
    NULL, NULL, zcr_write, NULL, raw_write
};

void hw_watchpoint_update(ARMCPU *cpu, int n)
{
    CPUARMState *env = &cpu->env;
    vaddr len = 0;
    vaddr wvr = env->cp15.dbgwvr[n];
    uint64_t wcr = env->cp15.dbgwcr[n];
    int mask;
    int flags = BP_CPU | BP_STOP_BEFORE_ACCESS;

    if (env->cpu_watchpoint[n]) {
        cpu_watchpoint_remove_by_ref(CPU(cpu), env->cpu_watchpoint[n]);
        env->cpu_watchpoint[n] = NULL;
    }

    if (!extract64(wcr, 0, 1)) {
        /* E bit clear : watchpoint disabled */
        return;
    }

    switch (extract64(wcr, 3, 2)) {
    case 0:
        /* LSC 00 is reserved and must behave as if the wp is disabled */
        return;
    case 1:
        flags |= BP_MEM_READ;
        break;
    case 2:
        flags |= BP_MEM_WRITE;
        break;
    case 3:
        flags |= BP_MEM_ACCESS;
        break;
    }

    /* Attempts to use both MASK and BAS fields simultaneously are
     * CONSTRAINED UNPREDICTABLE; we opt to ignore BAS in this case,
     * thus generating a watchpoint for every byte in the masked region.
     */
    mask = extract64(wcr, 24, 4);
    if (mask == 1 || mask == 2) {
        /* Reserved values of MASK; we must act as if the mask value was
         * some non-reserved value, or as if the watchpoint were disabled.
         * We choose the latter.
         */
        return;
    } else if (mask) {
        /* Watchpoint covers an aligned area up to 2GB in size */
        len = 1ULL << mask;
        /* If masked bits in WVR are not zero it's CONSTRAINED UNPREDICTABLE
         * whether the watchpoint fires when the unmasked bits match; we opt
         * to generate the exceptions.
         */
        wvr &= ~(len - 1);
    } else {
        /* Watchpoint covers bytes defined by the byte address select bits */
        int bas = extract64(wcr, 5, 8);
        int basstart;

        if (bas == 0) {
            /* This must act as if the watchpoint is disabled */
            return;
        }

        if (extract64(wvr, 2, 1)) {
            /* Deprecated case of an only 4-aligned address. BAS[7:4] are
             * ignored, and BAS[3:0] define which bytes to watch.
             */
            bas &= 0xf;
        }
        /* The BAS bits are supposed to be programmed to indicate a contiguous
         * range of bytes. Otherwise it is CONSTRAINED UNPREDICTABLE whether
         * we fire for each byte in the word/doubleword addressed by the WVR.
         * We choose to ignore any non-zero bits after the first range of 1s.
         */
        basstart = ctz32(bas);
        len = cto32(bas >> basstart);
        wvr += basstart;
    }

    cpu_watchpoint_insert(CPU(cpu), wvr, len, flags,
                          &env->cpu_watchpoint[n]);
}

void hw_watchpoint_update_all(ARMCPU *cpu)
{
    int i;
    CPUARMState *env = &cpu->env;

    /* Completely clear out existing QEMU watchpoints and our array, to
     * avoid possible stale entries following migration load.
     */
    cpu_watchpoint_remove_all(CPU(cpu), BP_CPU);
    memset(env->cpu_watchpoint, 0, sizeof(env->cpu_watchpoint));

    for (i = 0; i < ARRAY_SIZE(cpu->env.cpu_watchpoint); i++) {
        hw_watchpoint_update(cpu, i);
    }
}

static void dbgwvr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    int i = ri->crm;

    /* Bits [63:49] are hardwired to the value of bit [48]; that is, the
     * register reads and behaves as if values written are sign extended.
     * Bits [1:0] are RES0.
     */
    value = sextract64(value, 0, 49) & ~3ULL;

    raw_write(env, ri, value);
    hw_watchpoint_update(cpu, i);
}

static void dbgwcr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    int i = ri->crm;

    raw_write(env, ri, value);
    hw_watchpoint_update(cpu, i);
}

void hw_breakpoint_update(ARMCPU *cpu, int n)
{
    CPUARMState *env = &cpu->env;
    uint64_t bvr = env->cp15.dbgbvr[n];
    uint64_t bcr = env->cp15.dbgbcr[n];
    vaddr addr;
    int bt;
    int flags = BP_CPU;

    if (env->cpu_breakpoint[n]) {
        cpu_breakpoint_remove_by_ref(CPU(cpu), env->cpu_breakpoint[n]);
        env->cpu_breakpoint[n] = NULL;
    }

    if (!extract64(bcr, 0, 1)) {
        /* E bit clear : watchpoint disabled */
        return;
    }

    bt = extract64(bcr, 20, 4);

    switch (bt) {
    case 4: /* unlinked address mismatch (reserved if AArch64) */
    case 5: /* linked address mismatch (reserved if AArch64) */
        qemu_log_mask(LOG_UNIMP,
                      "arm: address mismatch breakpoint types not implemented");
        return;
    case 0: /* unlinked address match */
    case 1: /* linked address match */
    {
        /* Bits [63:49] are hardwired to the value of bit [48]; that is,
         * we behave as if the register was sign extended. Bits [1:0] are
         * RES0. The BAS field is used to allow setting breakpoints on 16
         * bit wide instructions; it is CONSTRAINED UNPREDICTABLE whether
         * a bp will fire if the addresses covered by the bp and the addresses
         * covered by the insn overlap but the insn doesn't start at the
         * start of the bp address range. We choose to require the insn and
         * the bp to have the same address. The constraints on writing to
         * BAS enforced in dbgbcr_write mean we have only four cases:
         *  0b0000  => no breakpoint
         *  0b0011  => breakpoint on addr
         *  0b1100  => breakpoint on addr + 2
         *  0b1111  => breakpoint on addr
         * See also figure D2-3 in the v8 ARM ARM (DDI0487A.c).
         */
        int bas = extract64(bcr, 5, 4);
        addr = sextract64(bvr, 0, 49) & ~3ULL;
        if (bas == 0) {
            return;
        }
        if (bas == 0xc) {
            addr += 2;
        }
        break;
    }
    case 2: /* unlinked context ID match */
    case 8: /* unlinked VMID match (reserved if no EL2) */
    case 10: /* unlinked context ID and VMID match (reserved if no EL2) */
        qemu_log_mask(LOG_UNIMP,
                      "arm: unlinked context breakpoint types not implemented");
        return;
    case 9: /* linked VMID match (reserved if no EL2) */
    case 11: /* linked context ID and VMID match (reserved if no EL2) */
    case 3: /* linked context ID match */
    default:
        /* We must generate no events for Linked context matches (unless
         * they are linked to by some other bp/wp, which is handled in
         * updates for the linking bp/wp). We choose to also generate no events
         * for reserved values.
         */
        return;
    }

    cpu_breakpoint_insert(CPU(cpu), addr, flags, &env->cpu_breakpoint[n]);
}

void hw_breakpoint_update_all(ARMCPU *cpu)
{
    int i;
    CPUARMState *env = &cpu->env;

    /* Completely clear out existing QEMU breakpoints and our array, to
     * avoid possible stale entries following migration load.
     */
    cpu_breakpoint_remove_all(CPU(cpu), BP_CPU);
    memset(env->cpu_breakpoint, 0, sizeof(env->cpu_breakpoint));

    for (i = 0; i < ARRAY_SIZE(cpu->env.cpu_breakpoint); i++) {
        hw_breakpoint_update(cpu, i);
    }
}

static void dbgbvr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    int i = ri->crm;

    raw_write(env, ri, value);
    hw_breakpoint_update(cpu, i);
}

static void dbgbcr_write(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    int i = ri->crm;

    /* BAS[3] is a read-only copy of BAS[2], and BAS[1] a read-only
     * copy of BAS[0].
     */
    value = deposit64(value, 6, 1, extract64(value, 5, 1));
    value = deposit64(value, 8, 1, extract64(value, 7, 1));

    raw_write(env, ri, value);
    hw_breakpoint_update(cpu, i);
}

static void define_debug_regs(ARMCPU *cpu)
{
    /* Define v7 and v8 architectural debug registers.
     * These are just dummy implementations for now.
     */
    int i;
    int wrps, brps, ctx_cmps;
    ARMCPRegInfo dbgdidr = {
        "DBGDIDR", 14,0,0, 0,0,0, 0,
        ARM_CP_CONST, PL0_R, 0, NULL, cpu->dbgdidr, 0, {0, 0},
        access_tda
    };

    /* Note that all these register fields hold "number of Xs minus 1". */
    brps = extract32(cpu->dbgdidr, 24, 4);
    wrps = extract32(cpu->dbgdidr, 28, 4);
    ctx_cmps = extract32(cpu->dbgdidr, 20, 4);

    assert(ctx_cmps <= brps);

    /* The DBGDIDR and ID_AA64DFR0_EL1 define various properties
     * of the debug registers such as number of breakpoints;
     * check that if they both exist then they agree.
     */
    if (arm_feature(&cpu->env, ARM_FEATURE_AARCH64)) {
        assert(extract32(cpu->id_aa64dfr0, 12, 4) == brps);
        assert(extract32(cpu->id_aa64dfr0, 20, 4) == wrps);
        assert(extract32(cpu->id_aa64dfr0, 28, 4) == ctx_cmps);
    }

    define_one_arm_cp_reg(cpu, &dbgdidr);
    define_arm_cp_regs(cpu, debug_cp_reginfo);

    if (arm_feature(&cpu->env, ARM_FEATURE_LPAE)) {
        define_arm_cp_regs(cpu, debug_lpae_cp_reginfo);
    }

    for (i = 0; i < brps + 1; i++) {
        ARMCPRegInfo dbgregs[] = {
            { "DBGBVR", 14,0,i, 2,0,4,ARM_CP_STATE_BOTH,
              0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.dbgbvr[i]), {0, 0},
              access_tda, NULL,dbgbvr_write, NULL,raw_write
            },
            { "DBGBCR", 14,0,i, 2,0,5, ARM_CP_STATE_BOTH,
              0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.dbgbcr[i]), {0, 0},
              access_tda, NULL,dbgbcr_write, NULL,raw_write
            },
            REGINFO_SENTINEL
        };
        define_arm_cp_regs(cpu, dbgregs);
    }

    for (i = 0; i < wrps + 1; i++) {
        ARMCPRegInfo dbgregs[] = {
            { "DBGWVR", 14,0,i, 2,0,6, ARM_CP_STATE_BOTH,
              0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.dbgwvr[i]), {0, 0},
              access_tda, NULL,dbgwvr_write, NULL,raw_write
            },
            { "DBGWCR", 14,0,i, 2,0,7, ARM_CP_STATE_BOTH,
              0, PL1_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.dbgwcr[i]), {0, 0},
              access_tda, NULL,dbgwcr_write, NULL,raw_write
            },
            REGINFO_SENTINEL
        };
        define_arm_cp_regs(cpu, dbgregs);
    }
}

/* We don't know until after realize whether there's a GICv3
 * attached, and that is what registers the gicv3 sysregs.
 * So we have to fill in the GIC fields in ID_PFR/ID_PFR1_EL1/ID_AA64PFR0_EL1
 * at runtime.
 */
static uint64_t id_pfr1_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    uint64_t pfr1 = cpu->id_pfr1;

    if (env->gicv3state) {
        pfr1 |= 1 << 28;
    }
    return pfr1;
}

static uint64_t id_aa64pfr0_read(CPUARMState *env, const ARMCPRegInfo *ri)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    uint64_t pfr0 = cpu->id_aa64pfr0;

    if (env->gicv3state) {
        pfr0 |= 1 << 24;
    }
    return pfr0;
}

void register_cp_regs_for_features(ARMCPU *cpu)
{
    /* Register all the coprocessor registers based on feature bits */
    CPUARMState *env = &cpu->env;
    if (arm_feature(env, ARM_FEATURE_M)) {
        /* M profile has no coprocessor registers */
        return;
    }

    define_arm_cp_regs(cpu, cp_reginfo);
    if (!arm_feature(env, ARM_FEATURE_V8)) {
        /* Must go early as it is full of wildcards that may be
         * overridden by later definitions.
         */
        define_arm_cp_regs(cpu, not_v8_cp_reginfo);
    }

    if (arm_feature(env, ARM_FEATURE_V6)) {
        /* The ID registers all have impdef reset values */
        ARMCPRegInfo v6_idregs[] = {
            { "ID_PFR0", 0,0,1, 3,0,0, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_pfr0 },
            /* ID_PFR1 is not a plain ARM_CP_CONST because we don't know
             * the value of the GIC field until after we define these regs.
             */
            { "ID_PFR1", 0,0,1, 3,0,1, ARM_CP_STATE_BOTH,
              ARM_CP_NO_RAW, PL1_R, 0, NULL, cpu->id_pfr1, 0, {0, 0},
              NULL, id_pfr1_read, arm_cp_write_ignore },
            { "ID_DFR0", 0,0,1, 3,0,2, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_dfr0 },
            { "ID_AFR0", 0,0,1, 3,0,3, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_afr0 },
            { "ID_MMFR0", 0,0,1, 3,0,4, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_mmfr0 },
            { "ID_MMFR1", 0,0,1, 3,0,5, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_mmfr1 },
            { "ID_MMFR2", 0,0,1, 3,0,6, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_mmfr2 },
            { "ID_MMFR3", 0,0,1, 3,0,7, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_mmfr3 },
            { "ID_ISAR0", 0,0,2, 3,0,0, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_isar0 },
            { "ID_ISAR1", 0,0,2, 3,0,1, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_isar1 },
            { "ID_ISAR2", 0,0,2, 3,0,2, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_isar2 },
            { "ID_ISAR3", 0,0,2, 3,0,3, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_isar3 },
            { "ID_ISAR4", 0,0,2, 3,0,4, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_isar4 },
            { "ID_ISAR5", 0,0,2, 3,0,5, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_isar5 },
            /* 6..7 are as yet unallocated and must RAZ */
            { "ID_ISAR6", 15,0,2, 0,0,6, 0,
              ARM_CP_CONST, PL1_R, 0, NULL, 0 },
            { "ID_ISAR7", 15,0,2, 0,0,7, 0,
              ARM_CP_CONST, PL1_R, 0, NULL, 0 },
            REGINFO_SENTINEL
        };
        define_arm_cp_regs(cpu, v6_idregs);
        define_arm_cp_regs(cpu, v6_cp_reginfo);
    } else {
        define_arm_cp_regs(cpu, not_v6_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_V6K)) {
        define_arm_cp_regs(cpu, v6k_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_V7MP) &&
        !arm_feature(env, ARM_FEATURE_PMSA)) {
        define_arm_cp_regs(cpu, v7mp_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_V7)) {
        /* v7 performance monitor control register: same implementor
         * field as main ID register, and we implement only the cycle
         * count register.
         */
#ifndef CONFIG_USER_ONLY
        ARMCPRegInfo pmcr = {
            "PMCR", 15,9,12, 0,0,0, 0,
            ARM_CP_IO | ARM_CP_ALIAS, PL0_RW, 0, NULL, 0, offsetoflow32(CPUARMState, cp15.c9_pmcr), {0, 0},
            pmreg_access, NULL,pmcr_write, NULL,raw_write,
        };
        ARMCPRegInfo pmcr64 = {
            "PMCR_EL0", 0,9,12, 3,3,0, ARM_CP_STATE_AA64,
            ARM_CP_IO, PL0_RW, 0, NULL, cpu->midr & 0xff000000, offsetof(CPUARMState, cp15.c9_pmcr), {0, 0},
            pmreg_access, NULL,pmcr_write, NULL,raw_write,
        };
        define_one_arm_cp_reg(cpu, &pmcr);
        define_one_arm_cp_reg(cpu, &pmcr64);
#endif
        ARMCPRegInfo clidr = {
            "CLIDR", 0,0,0, 3,1,1, ARM_CP_STATE_BOTH,
            ARM_CP_CONST, PL1_R, 0, NULL, cpu->clidr
        };
        define_one_arm_cp_reg(cpu, &clidr);
        define_arm_cp_regs(cpu, v7_cp_reginfo);
        define_debug_regs(cpu);
    } else {
        define_arm_cp_regs(cpu, not_v7_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_V8)) {
        /* AArch64 ID registers, which all have impdef reset values.
         * Note that within the ID register ranges the unused slots
         * must all RAZ, not UNDEF; future architecture versions may
         * define new registers here.
         */
        ARMCPRegInfo v8_idregs[] = {
            /* ID_AA64PFR0_EL1 is not a plain ARM_CP_CONST because we don't
             * know the right value for the GIC field until after we
             * define these regs.
             */
            { "ID_AA64PFR0_EL1", 0,0,4, 3,0,0, ARM_CP_STATE_AA64,
              ARM_CP_NO_RAW, PL1_R, 0, NULL, cpu->id_aa64pfr0, 0, {0, 0},
              NULL, id_aa64pfr0_read, arm_cp_write_ignore },
            { "ID_AA64PFR1_EL1", 0,0,4, 3,0,1, ARM_CP_STATE_AA64,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_aa64pfr1},
            { "ID_AA64PFR2_EL1_RESERVED", 0,0,4, 3,0,2, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64PFR3_EL1_RESERVED", 0,0,4, 3,0,3, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0,},
            { "ID_AA64PFR4_EL1_RESERVED", 0,0,4, 3,0,4, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64PFR5_EL1_RESERVED", 0,0,4, 3,0,5, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64PFR6_EL1_RESERVED", 0,0,4, 3,0,6, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64PFR7_EL1_RESERVED", 0,0,4, 3,0,7, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64DFR0_EL1", 0,0,5, 3,0,0, ARM_CP_STATE_AA64,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_aa64dfr0 },
            { "ID_AA64DFR1_EL1", 0,0,5, 3,0,1, ARM_CP_STATE_AA64,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_aa64dfr1 },
            { "ID_AA64DFR2_EL1_RESERVED", 0,0,5, 3,0,2, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64DFR3_EL1_RESERVED", 0,0,5, 3,0,3, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64AFR0_EL1", 0,0,5, 3,0,4, ARM_CP_STATE_AA64,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_aa64afr0 },
            { "ID_AA64AFR1_EL1", 0,0,5, 3,0,5, ARM_CP_STATE_AA64,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_aa64afr1 },
            { "ID_AA64AFR2_EL1_RESERVED", 0,0,5, 3,0,6, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64AFR3_EL1_RESERVED", 0,0,5, 3,0,7, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64ISAR0_EL1", 0,0,6, 3,0,0, ARM_CP_STATE_AA64,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_aa64isar0 },
            { "ID_AA64ISAR1_EL1", 0,0,6, 3,0,1, ARM_CP_STATE_AA64,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_aa64isar1 },
            { "ID_AA64ISAR2_EL1_RESERVED", 0,0,6, 3,0,2, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64ISAR3_EL1_RESERVED", 0,0,6, 3,0,3, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64ISAR4_EL1_RESERVED", 0,0,6, 3,0,4, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64ISAR5_EL1_RESERVED", 0,0,6, 3,0,5, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64ISAR6_EL1_RESERVED", 0,0,6, 3,0,6, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64ISAR7_EL1_RESERVED", 0,0,6, 3,0,7, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64MMFR0_EL1", 0,0,7, 3,0,0, ARM_CP_STATE_AA64,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_aa64mmfr0 },
            { "ID_AA64MMFR1_EL1", 0,0,7, 3,0,1, ARM_CP_STATE_AA64,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->id_aa64mmfr1 },
            { "ID_AA64MMFR2_EL1_RESERVED", 0,0,7, 3,0,2, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64MMFR3_EL1_RESERVED", 0,0,7, 3,0,3, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64MMFR4_EL1_RESERVED", 0,0,7, 3,0,4, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64MMFR5_EL1_RESERVED", 0,0,7, 3,0,5, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64MMFR6_EL1_RESERVED", 0,0,7, 3,0,6, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "ID_AA64MMFR7_EL1_RESERVED", 0,0,7, 3,0,7, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "MVFR0_EL1", 0,0,3, 3,0,0, ARM_CP_STATE_AA64,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->mvfr0 },
            { "MVFR1_EL1", 0,0,3, 3,0,1, ARM_CP_STATE_AA64,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->mvfr1 },
            { "MVFR2_EL1", 0,0,3, 3,0,2, ARM_CP_STATE_AA64,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->mvfr2 },
            { "MVFR3_EL1_RESERVED", 0,0,3, 3,0,3, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "MVFR4_EL1_RESERVED", 0,0,3, 3,0,4, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "MVFR5_EL1_RESERVED", 0,0,3, 3,0,5, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "MVFR6_EL1_RESERVED", 0,0,3, 3,0,6, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "MVFR7_EL1_RESERVED", 0,0,3, 3,0,7, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL1_R, 0, NULL, 0 },
            { "PMCEID0", 15,9,12, 0,0,6, ARM_CP_STATE_AA32, ARM_CP_CONST,
              PL0_R, 0, NULL, cpu->pmceid0, 0, {0, 0},
              pmreg_access },
            { "PMCEID0_EL0", 0,9,12, 3,3,6, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL0_R, 0, NULL, cpu->pmceid0, 0, {0, 0},
              pmreg_access },
            { "PMCEID1", 15,9,12, 0,0,7, ARM_CP_STATE_AA32, ARM_CP_CONST,
              PL0_R, 0, NULL, cpu->pmceid1, 0, {0, 0},
              pmreg_access },
            { "PMCEID1_EL0", 0,9,12, 3,3,7, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL0_R, 0, NULL, cpu->pmceid1, 0, {0, 0},
              pmreg_access },
            REGINFO_SENTINEL
        };
        /* RVBAR_EL1 is only implemented if EL1 is the highest EL */
        if (!arm_feature(env, ARM_FEATURE_EL3) &&
            !arm_feature(env, ARM_FEATURE_EL2)) {
            ARMCPRegInfo rvbar = {
                "RVBAR_EL1", 0,12,0, 3,0,1, ARM_CP_STATE_AA64,
                ARM_CP_CONST, PL1_R, 0, NULL, cpu->rvbar
            };
            define_one_arm_cp_reg(cpu, &rvbar);
        }
        define_arm_cp_regs(cpu, v8_idregs);
        define_arm_cp_regs(cpu, v8_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_EL2)) {
        uint64_t vmpidr_def = mpidr_read_val(env);
        ARMCPRegInfo vpidr_regs[] = {
            { "VPIDR", 15,0,0, 0,4,0, ARM_CP_STATE_AA32, ARM_CP_ALIAS,
              PL2_RW, 0, NULL, cpu->midr, offsetoflow32(CPUARMState, cp15.vpidr_el2), {0, 0},
              access_el3_aa32ns },
            { "VPIDR_EL2", 0,0,0, 3,4,0, ARM_CP_STATE_AA64, 0,
              PL2_RW, 0, NULL, cpu->midr, offsetof(CPUARMState, cp15.vpidr_el2) },
            { "VMPIDR", 15,0,0, 0,4,5, ARM_CP_STATE_AA32, ARM_CP_ALIAS,
              PL2_RW, 0, NULL, vmpidr_def, offsetoflow32(CPUARMState, cp15.vmpidr_el2), {0, 0},
              access_el3_aa32ns },
            { "VMPIDR_EL2", 0,0,0, 3,4,5, ARM_CP_STATE_AA64, 0,
              PL2_RW, 0, NULL, vmpidr_def, offsetof(CPUARMState, cp15.vmpidr_el2) },
            REGINFO_SENTINEL
        };
        define_arm_cp_regs(cpu, vpidr_regs);
        define_arm_cp_regs(cpu, el2_cp_reginfo);
        /* RVBAR_EL2 is only implemented if EL2 is the highest EL */
        if (!arm_feature(env, ARM_FEATURE_EL3)) {
            ARMCPRegInfo rvbar = {
                "RVBAR_EL2", 0,12,0, 3,4,1, ARM_CP_STATE_AA64,
                ARM_CP_CONST, PL2_R, 0, NULL, cpu->rvbar
            };
            define_one_arm_cp_reg(cpu, &rvbar);
        }
    } else {
        /* If EL2 is missing but higher ELs are enabled, we need to
         * register the no_el2 reginfos.
         */
        if (arm_feature(env, ARM_FEATURE_EL3)) {
            /* When EL3 exists but not EL2, VPIDR and VMPIDR take the value
             * of MIDR_EL1 and MPIDR_EL1.
             */
            ARMCPRegInfo vpidr_regs[] = {
                { "VPIDR_EL2", 0,0,0, 3,4,0, ARM_CP_STATE_BOTH, ARM_CP_CONST,
                  PL2_RW, 0, NULL, cpu->midr, offsetof(CPUARMState, cp15.vpidr_el2), {0, 0},
                  access_el3_aa32ns_aa64any },
                { "VMPIDR_EL2", 0,0,0, 3,4,5, ARM_CP_STATE_BOTH, ARM_CP_NO_RAW,
                  PL2_RW, 0, NULL, 0, 0, {0, 0},
                  access_el3_aa32ns_aa64any, mpidr_read, arm_cp_write_ignore },
                REGINFO_SENTINEL
            };
            define_arm_cp_regs(cpu, vpidr_regs);
            define_arm_cp_regs(cpu, el3_no_el2_cp_reginfo);
        }
    }
    if (arm_feature(env, ARM_FEATURE_EL3)) {
        define_arm_cp_regs(cpu, el3_cp_reginfo);
        ARMCPRegInfo el3_regs[] = {
            { "RVBAR_EL3", 0,12,0, 3,6,1, ARM_CP_STATE_AA64,
                ARM_CP_CONST, PL3_R, 0, NULL, cpu->rvbar },
            { "SCTLR_EL3", 0,1,0, 3,6,0, ARM_CP_STATE_AA64, 0,
              PL3_RW, 0, NULL, 0, offsetof(CPUARMState, cp15.sctlr_el[3]), {0, 0},
              NULL, NULL, sctlr_write, NULL, raw_write, },
        };

        define_arm_cp_regs(cpu, el3_regs);
    }
    /* The behaviour of NSACR is sufficiently various that we don't
     * try to describe it in a single reginfo:
     *  if EL3 is 64 bit, then trap to EL3 from S EL1,
     *     reads as constant 0xc00 from NS EL1 and NS EL2
     *  if EL3 is 32 bit, then RW at EL3, RO at NS EL1 and NS EL2
     *  if v7 without EL3, register doesn't exist
     *  if v8 without EL3, reads as constant 0xc00 from NS EL1 and NS EL2
     */
    if (arm_feature(env, ARM_FEATURE_EL3)) {
        if (arm_feature(env, ARM_FEATURE_AARCH64)) {
            ARMCPRegInfo nsacr = {
                "NSACR", 15,1,1, 0,0,2, 0, ARM_CP_CONST,
                PL1_RW, 0, NULL, 0xc00, 0, {0, 0},
                nsacr_access
            };
            define_one_arm_cp_reg(cpu, &nsacr);
        } else {
            ARMCPRegInfo nsacr = {
                "NSACR", 15,1,1, 0,0,2, 0,0,
                PL3_RW | PL1_R, 0, NULL, 0, offsetof(CPUARMState, cp15.nsacr)
            };
            define_one_arm_cp_reg(cpu, &nsacr);
        }
    } else {
        if (arm_feature(env, ARM_FEATURE_V8)) {
            ARMCPRegInfo nsacr = {
                "NSACR", 15,1,1, 0,0,2, 0, ARM_CP_CONST,
                PL1_R, 0, NULL, 0xc00
            };
            define_one_arm_cp_reg(cpu, &nsacr);
        }
    }

    if (arm_feature(env, ARM_FEATURE_PMSA)) {
        if (arm_feature(env, ARM_FEATURE_V6)) {
            /* PMSAv6 not implemented */
            assert(arm_feature(env, ARM_FEATURE_V7));
            define_arm_cp_regs(cpu, vmsa_pmsa_cp_reginfo);
            define_arm_cp_regs(cpu, pmsav7_cp_reginfo);
        } else {
            define_arm_cp_regs(cpu, pmsav5_cp_reginfo);
        }
    } else {
        define_arm_cp_regs(cpu, vmsa_pmsa_cp_reginfo);
        define_arm_cp_regs(cpu, vmsa_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_THUMB2EE)) {
        define_arm_cp_regs(cpu, t2ee_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_GENERIC_TIMER)) {
        define_arm_cp_regs(cpu, generic_timer_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_VAPA)) {
        define_arm_cp_regs(cpu, vapa_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_CACHE_TEST_CLEAN)) {
        define_arm_cp_regs(cpu, cache_test_clean_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_CACHE_DIRTY_REG)) {
        define_arm_cp_regs(cpu, cache_dirty_status_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_CACHE_BLOCK_OPS)) {
        define_arm_cp_regs(cpu, cache_block_ops_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_OMAPCP)) {
        define_arm_cp_regs(cpu, omap_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_STRONGARM)) {
        define_arm_cp_regs(cpu, strongarm_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_XSCALE)) {
        define_arm_cp_regs(cpu, xscale_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_DUMMY_C15_REGS)) {
        define_arm_cp_regs(cpu, dummy_c15_cp_reginfo);
    }
    if (arm_feature(env, ARM_FEATURE_LPAE)) {
        define_arm_cp_regs(cpu, lpae_cp_reginfo);
    }
    /* Slightly awkwardly, the OMAP and StrongARM cores need all of
     * cp15 crn=0 to be writes-ignored, whereas for other cores they should
     * be read-only (ie write causes UNDEF exception).
     */
    {
        ARMCPRegInfo id_pre_v8_midr_cp_reginfo[] = {
            /* Pre-v8 MIDR space.
             * Note that the MIDR isn't a simple constant register because
             * of the TI925 behaviour where writes to another register can
             * cause the MIDR value to change.
             *
             * Unimplemented registers in the c15 0 0 0 space default to
             * MIDR. Define MIDR first as this entire space, then CTR, TCMTR
             * and friends override accordingly.
             */
            { "MIDR", 15,0,0, 0,0,CP_ANY, 0,
              ARM_CP_OVERRIDE, PL1_R, 0, NULL, cpu->midr, offsetof(CPUARMState, cp15.c0_cpuid), {0, 0},
              NULL, midr_read, arm_cp_write_ignore, NULL, raw_write, },
            /* crn = 0 op1 = 0 crm = 3..7 : currently unassigned; we RAZ. */
            { "DUMMY",
              15,0,3, 0,0,CP_ANY, 0,
              ARM_CP_CONST, PL1_R, 0, NULL, 0 },
            { "DUMMY",
              15,0,4, 0,0,CP_ANY, 0,
              ARM_CP_CONST, PL1_R, 0, NULL, 0 },
            { "DUMMY",
              15,0,5, 0,0,CP_ANY, 0,
              ARM_CP_CONST, PL1_R, 0, NULL, 0 },
            { "DUMMY",
              15,0,6, 0,0,CP_ANY, 0,
              ARM_CP_CONST, PL1_R, 0, NULL, 0 },
            { "DUMMY",
              15,0,7, 0,0,CP_ANY, 0,
              ARM_CP_CONST, PL1_R, 0, NULL, 0 },
            REGINFO_SENTINEL
        };
        ARMCPRegInfo id_v8_midr_cp_reginfo[] = {
            { "MIDR_EL1", 0,0,0, 3,0,0, ARM_CP_STATE_BOTH,
              ARM_CP_NO_RAW, PL1_R, 0, NULL, cpu->midr, offsetof(CPUARMState, cp15.c0_cpuid), {0, 0},
              NULL, midr_read },
            /* crn = 0 op1 = 0 crm = 0 op2 = 4,7 : AArch32 aliases of MIDR */
            { "MIDR", 15,0,0, 0,0,4, 0, ARM_CP_ALIAS | ARM_CP_CONST,
              PL1_R, 0, NULL, cpu->midr },
            { "MIDR", 15,0,0, 0,0,7, 0, ARM_CP_ALIAS | ARM_CP_CONST,
              PL1_R, 0, NULL, cpu->midr },
            { "REVIDR_EL1", 0,0,0, 3,0,6, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->revidr },
            REGINFO_SENTINEL
        };
        ARMCPRegInfo id_cp_reginfo[] = {
            /* These are common to v8 and pre-v8 */
            { "CTR", 15,0,0, 0,0,1, 0,
              ARM_CP_CONST, PL1_R, 0, NULL, cpu->ctr },
            { "CTR_EL0", 0,0,0, 3,3,1, ARM_CP_STATE_AA64,
             ARM_CP_CONST, PL0_R, 0, NULL, cpu->ctr, 0, {0, 0},
             ctr_el0_access, },
            /* TCMTR and TLBTR exist in v8 but have no 64-bit versions */
            { "TCMTR", 15,0,0, 0,0,2, 0,
              ARM_CP_CONST, PL1_R, 0, NULL, 0 },
            REGINFO_SENTINEL
        };
        /* TLBTR is specific to VMSA */
        ARMCPRegInfo id_tlbtr_reginfo = {
            "TLBTR", 15,0,0, 0,0,3, 0,
            ARM_CP_CONST, PL1_R, 0, NULL, 0,
        };
        /* MPUIR is specific to PMSA V6+ */
        ARMCPRegInfo id_mpuir_reginfo = {
              "MPUIR", 15,0,0, 0,0,4, 0,ARM_CP_CONST,
              PL1_R, 0, NULL, cpu->pmsav7_dregion << 8
        };
        ARMCPRegInfo crn0_wi_reginfo = {
            "CRN0_WI", 15,0,CP_ANY, 0,CP_ANY,CP_ANY, 0,
            ARM_CP_NOP | ARM_CP_OVERRIDE, PL1_W,
        };
        if (arm_feature(env, ARM_FEATURE_OMAPCP) ||
            arm_feature(env, ARM_FEATURE_STRONGARM)) {
            ARMCPRegInfo *r;
            /* Register the blanket "writes ignored" value first to cover the
             * whole space. Then update the specific ID registers to allow write
             * access, so that they ignore writes rather than causing them to
             * UNDEF.
             */
            define_one_arm_cp_reg(cpu, &crn0_wi_reginfo);
            for (r = id_pre_v8_midr_cp_reginfo;
                 r->type != ARM_CP_SENTINEL; r++) {
                r->access = PL1_RW;
            }
            for (r = id_cp_reginfo; r->type != ARM_CP_SENTINEL; r++) {
                r->access = PL1_RW;
            }
            id_tlbtr_reginfo.access = PL1_RW;
            id_tlbtr_reginfo.access = PL1_RW;
        }
        if (arm_feature(env, ARM_FEATURE_V8)) {
            define_arm_cp_regs(cpu, id_v8_midr_cp_reginfo);
        } else {
            define_arm_cp_regs(cpu, id_pre_v8_midr_cp_reginfo);
        }
        define_arm_cp_regs(cpu, id_cp_reginfo);
        if (!arm_feature(env, ARM_FEATURE_PMSA)) {
            define_one_arm_cp_reg(cpu, &id_tlbtr_reginfo);
        } else if (arm_feature(env, ARM_FEATURE_V7)) {
            define_one_arm_cp_reg(cpu, &id_mpuir_reginfo);
        }
    }

    if (arm_feature(env, ARM_FEATURE_MPIDR)) {
        define_arm_cp_regs(cpu, mpidr_cp_reginfo);
    }

    if (arm_feature(env, ARM_FEATURE_AUXCR)) {
        ARMCPRegInfo auxcr_reginfo[] = {
            { "ACTLR_EL1", 0,1,0, 3,0,1, ARM_CP_STATE_BOTH,
              ARM_CP_CONST, PL1_RW, 0, NULL, cpu->reset_auxcr },
            { "ACTLR_EL2",0,1,0, 3,4,1, ARM_CP_STATE_BOTH, ARM_CP_CONST,
              PL2_RW, 0, NULL, 0 },
            { "ACTLR_EL3", 0,1,0, 3,6,1, ARM_CP_STATE_AA64, ARM_CP_CONST,
              PL3_RW, 0, NULL, 0 },
            REGINFO_SENTINEL
        };
        define_arm_cp_regs(cpu, auxcr_reginfo);
    }

    if (arm_feature(env, ARM_FEATURE_CBAR)) {
        if (arm_feature(env, ARM_FEATURE_AARCH64)) {
            /* 32 bit view is [31:18] 0...0 [43:32]. */
            uint32_t cbar32 = (extract64(cpu->reset_cbar, 18, 14) << 18)
                | extract64(cpu->reset_cbar, 32, 12);
            ARMCPRegInfo cbar_reginfo[] = {
                { "CBAR", 15,15,0, 0,4,0, 0,
                  ARM_CP_CONST, PL1_R, 0, NULL, cpu->reset_cbar },
                { "CBAR_EL1", 0,15,3, 3,1,0, ARM_CP_STATE_AA64,
                  ARM_CP_CONST, PL1_R, 0, NULL, cbar32 },
                REGINFO_SENTINEL
            };
            /* We don't implement a r/w 64 bit CBAR currently */
            assert(arm_feature(env, ARM_FEATURE_CBAR_RO));
            define_arm_cp_regs(cpu, cbar_reginfo);
        } else {
            ARMCPRegInfo cbar = {
                "CBAR", 15,15,0, 0,4,0, 0,
                0, PL1_R|PL3_W, 0, NULL, cpu->reset_cbar, offsetof(CPUARMState, cp15.c15_config_base_address)
            };
            if (arm_feature(env, ARM_FEATURE_CBAR_RO)) {
                cbar.access = PL1_R;
                cbar.fieldoffset = 0;
                cbar.type = ARM_CP_CONST;
            }
            define_one_arm_cp_reg(cpu, &cbar);
        }
    }

    if (arm_feature(env, ARM_FEATURE_VBAR)) {
        ARMCPRegInfo vbar_cp_reginfo[] = {
            { "VBAR", 0,12,0, 3,0,0, ARM_CP_STATE_BOTH, 0,
              PL1_RW, 0, NULL, 0, 0,
              { offsetof(CPUARMState, cp15.vbar_s),
                offsetof(CPUARMState, cp15.vbar_ns) },
              NULL, NULL, vbar_write },
            REGINFO_SENTINEL
        };
        define_arm_cp_regs(cpu, vbar_cp_reginfo);
    }

    /* Generic registers whose values depend on the implementation */
    {
        ARMCPRegInfo sctlr = {
            "SCTLR", 0,1,0, 3,0,0, ARM_CP_STATE_BOTH,
            0, PL1_RW, 0, NULL, cpu->reset_sctlr, 0,
            {offsetof(CPUARMState, cp15.sctlr_s), offsetof(CPUARMState, cp15.sctlr_ns)},
            NULL, NULL,sctlr_write, NULL,raw_write,
        };
        if (arm_feature(env, ARM_FEATURE_XSCALE)) {
            /* Normally we would always end the TB on an SCTLR write, but Linux
             * arch/arm/mach-pxa/sleep.S expects two instructions following
             * an MMU enable to execute from cache.  Imitate this behaviour.
             */
            sctlr.type |= ARM_CP_SUPPRESS_TB_END;
        }
        define_one_arm_cp_reg(cpu, &sctlr);
    }

    if (arm_feature(env, ARM_FEATURE_SVE)) {
        define_one_arm_cp_reg(cpu, &zcr_el1_reginfo);
        if (arm_feature(env, ARM_FEATURE_EL2)) {
            define_one_arm_cp_reg(cpu, &zcr_el2_reginfo);
        } else {
            define_one_arm_cp_reg(cpu, &zcr_no_el2_reginfo);
        }
        if (arm_feature(env, ARM_FEATURE_EL3)) {
            define_one_arm_cp_reg(cpu, &zcr_el3_reginfo);
        }
    }
}

ARMCPU *cpu_arm_init(struct uc_struct *uc, const char *cpu_model)
{
    return ARM_CPU(uc, cpu_generic_init(uc, TYPE_ARM_CPU, cpu_model));
}

void arm_cpu_register_gdb_regs_for_features(ARMCPU *cpu)
{
#if 0
    CPUState *cs = CPU(cpu);
    CPUARMState *env = &cpu->env;

    if (arm_feature(env, ARM_FEATURE_AARCH64)) {
        gdb_register_coprocessor(cs, aarch64_fpu_gdb_get_reg,
                                 aarch64_fpu_gdb_set_reg,
                                 34, "aarch64-fpu.xml", 0);
    } else if (arm_feature(env, ARM_FEATURE_NEON)) {
        gdb_register_coprocessor(cs, vfp_gdb_get_reg, vfp_gdb_set_reg,
                                 51, "arm-neon.xml", 0);
    } else if (arm_feature(env, ARM_FEATURE_VFP3)) {
        gdb_register_coprocessor(cs, vfp_gdb_get_reg, vfp_gdb_set_reg,
                                 35, "arm-vfp3.xml", 0);
    } else if (arm_feature(env, ARM_FEATURE_VFP)) {
        gdb_register_coprocessor(cs, vfp_gdb_get_reg, vfp_gdb_set_reg,
                                 19, "arm-vfp.xml", 0);
    }
#endif
}

/* Sort alphabetically by type name, except for "any". */
#if 0
static void arm_cpu_list_entry(gpointer data, gpointer user_data)
{
    ObjectClass *oc = data;
    CPUListState *s = user_data;
    const char *typename;
    char *name;

    typename = object_class_get_name(oc);
    name = g_strndup(typename, strlen(typename) - strlen("-" TYPE_ARM_CPU));
    (*s->cpu_fprintf)(s->file, "  %s\n",
                      name);
    g_free(name);
}
#endif

void arm_cpu_list(FILE *f, fprintf_function cpu_fprintf)
{
#if 0
    CPUListState s = {
        .file = f,
        .cpu_fprintf = cpu_fprintf,
    };
    GSList *list;

    list = object_class_get_list(TYPE_ARM_CPU, false);
    list = g_slist_sort(list, arm_cpu_list_compare);
    (*cpu_fprintf)(f, "Available CPUs:\n");
    g_slist_foreach(list, arm_cpu_list_entry, &s);
    g_slist_free(list);
#ifdef CONFIG_KVM
    /* The 'host' CPU type is dynamically registered only if KVM is
     * enabled, so we have to special-case it here:
     */
    (*cpu_fprintf)(f, "  host (only available in KVM mode)\n");
#endif
#endif
}

static void add_cpreg_to_hashtable(ARMCPU *cpu, const ARMCPRegInfo *r,
                                   void *opaque, int state, int secstate,
                                   int crm, int opc1, int opc2)
{
    /* Private utility function for define_one_arm_cp_reg_with_opaque():
     * add a single reginfo struct to the hash table.
     */
    uint32_t *key = g_new(uint32_t, 1);
    ARMCPRegInfo *r2 = g_memdup(r, sizeof(ARMCPRegInfo));
    int is64 = (r->type & ARM_CP_64BIT) ? 1 : 0;
    int ns = (secstate & ARM_CP_SECSTATE_NS) ? 1 : 0;

    /* Reset the secure state to the specific incoming state.  This is
     * necessary as the register may have been defined with both states.
     */
    r2->secure = secstate;

    if (r->bank_fieldoffsets[0] && r->bank_fieldoffsets[1]) {
        /* Register is banked (using both entries in array).
         * Overwriting fieldoffset as the array is only used to define
         * banked registers but later only fieldoffset is used.
         */
        r2->fieldoffset = r->bank_fieldoffsets[ns];
    }

    if (state == ARM_CP_STATE_AA32) {
        if (r->bank_fieldoffsets[0] && r->bank_fieldoffsets[1]) {
            /* If the register is banked then we don't need to migrate or
             * reset the 32-bit instance in certain cases:
             *
             * 1) If the register has both 32-bit and 64-bit instances then we
             *    can count on the 64-bit instance taking care of the
             *    non-secure bank.
             * 2) If ARMv8 is enabled then we can count on a 64-bit version
             *    taking care of the secure bank.  This requires that separate
             *    32 and 64-bit definitions are provided.
             */
            if ((r->state == ARM_CP_STATE_BOTH && ns) ||
                (arm_feature(&cpu->env, ARM_FEATURE_V8) && !ns)) {
                r2->type |= ARM_CP_ALIAS;
            }
        } else if ((secstate != r->secure) && !ns) {
            /* The register is not banked so we only want to allow migration of
             * the non-secure instance.
             */
            r2->type |= ARM_CP_ALIAS;
        }

        if (r->state == ARM_CP_STATE_BOTH) {
            /* We assume it is a cp15 register if the .cp field is left unset.
             */
            if (r2->cp == 0) {
                r2->cp = 15;
            }

#ifdef HOST_WORDS_BIGENDIAN
            if (r2->fieldoffset) {
                r2->fieldoffset += sizeof(uint32_t);
            }
#endif
        }
    }
    if (state == ARM_CP_STATE_AA64) {
        /* To allow abbreviation of ARMCPRegInfo
         * definitions, we treat cp == 0 as equivalent to
         * the value for "standard guest-visible sysreg".
         * STATE_BOTH definitions are also always "standard
         * sysreg" in their AArch64 view (the .cp value may
         * be non-zero for the benefit of the AArch32 view).
         */
        if (r->cp == 0 || r->state == ARM_CP_STATE_BOTH) {
            r2->cp = CP_REG_ARM64_SYSREG_CP;
        }
        *key = ENCODE_AA64_CP_REG(r2->cp, r2->crn, crm,
                                  r2->opc0, opc1, opc2);
    } else {
        *key = ENCODE_CP_REG(r2->cp, is64, ns, r2->crn, crm, opc1, opc2);
    }
    if (opaque) {
        r2->opaque = opaque;
    }
    /* reginfo passed to helpers is correct for the actual access,
     * and is never ARM_CP_STATE_BOTH:
     */
    r2->state = state;
    /* Make sure reginfo passed to helpers for wildcarded regs
     * has the correct crm/opc1/opc2 for this reg, not CP_ANY:
     */
    r2->crm = crm;
    r2->opc1 = opc1;
    r2->opc2 = opc2;
    /* By convention, for wildcarded registers only the first
     * entry is used for migration; the others are marked as
     * ALIAS so we don't try to transfer the register
     * multiple times. Special registers (ie NOP/WFI) are
     * never migratable and not even raw-accessible.
     */
    if ((r->type & ARM_CP_SPECIAL)) {
        r2->type |= ARM_CP_NO_RAW;
    }
    if (((r->crm == CP_ANY) && crm != 0) ||
        ((r->opc1 == CP_ANY) && opc1 != 0) ||
        ((r->opc2 == CP_ANY) && opc2 != 0)) {
        r2->type |= ARM_CP_ALIAS;
    }

    /* Check that raw accesses are either forbidden or handled. Note that
     * we can't assert this earlier because the setup of fieldoffset for
     * banked registers has to be done first.
     */
    if (!(r2->type & ARM_CP_NO_RAW)) {
        assert(!raw_accessors_invalid(r2));
    }

    /* Overriding of an existing definition must be explicitly
     * requested.
     */
    if (!(r->type & ARM_CP_OVERRIDE)) {
        ARMCPRegInfo *oldreg;
        oldreg = g_hash_table_lookup(cpu->cp_regs, key);
        if (oldreg && !(oldreg->type & ARM_CP_OVERRIDE)) {
            fprintf(stderr, "Register redefined: cp=%d %d bit "
                    "crn=%d crm=%d opc1=%d opc2=%d, "
                    "was %s, now %s\n", r2->cp, 32 + 32 * is64,
                    r2->crn, r2->crm, r2->opc1, r2->opc2,
                    oldreg->name, r2->name);
            g_assert_not_reached();
        }
    }
    g_hash_table_insert(cpu->cp_regs, key, r2);
}


void define_one_arm_cp_reg_with_opaque(ARMCPU *cpu,
                                       const ARMCPRegInfo *r, void *opaque)
{
    /* Define implementations of coprocessor registers.
     * We store these in a hashtable because typically
     * there are less than 150 registers in a space which
     * is 16*16*16*8*8 = 262144 in size.
     * Wildcarding is supported for the crm, opc1 and opc2 fields.
     * If a register is defined twice then the second definition is
     * used, so this can be used to define some generic registers and
     * then override them with implementation specific variations.
     * At least one of the original and the second definition should
     * include ARM_CP_OVERRIDE in its type bits -- this is just a guard
     * against accidental use.
     *
     * The state field defines whether the register is to be
     * visible in the AArch32 or AArch64 execution state. If the
     * state is set to ARM_CP_STATE_BOTH then we synthesise a
     * reginfo structure for the AArch32 view, which sees the lower
     * 32 bits of the 64 bit register.
     *
     * Only registers visible in AArch64 may set r->opc0; opc0 cannot
     * be wildcarded. AArch64 registers are always considered to be 64
     * bits; the ARM_CP_64BIT* flag applies only to the AArch32 view of
     * the register, if any.
     */
    int crm, opc1, opc2, state;
    int crmmin = (r->crm == CP_ANY) ? 0 : r->crm;
    int crmmax = (r->crm == CP_ANY) ? 15 : r->crm;
    int opc1min = (r->opc1 == CP_ANY) ? 0 : r->opc1;
    int opc1max = (r->opc1 == CP_ANY) ? 7 : r->opc1;
    int opc2min = (r->opc2 == CP_ANY) ? 0 : r->opc2;
    int opc2max = (r->opc2 == CP_ANY) ? 7 : r->opc2;
    /* 64 bit registers have only CRm and Opc1 fields */
    assert(!((r->type & ARM_CP_64BIT) && (r->opc2 || r->crn)));
    /* op0 only exists in the AArch64 encodings */
    assert((r->state != ARM_CP_STATE_AA32) || (r->opc0 == 0));
    /* AArch64 regs are all 64 bit so ARM_CP_64BIT is meaningless */
    assert((r->state != ARM_CP_STATE_AA64) || !(r->type & ARM_CP_64BIT));
    /* The AArch64 pseudocode CheckSystemAccess() specifies that op1
     * encodes a minimum access level for the register. We roll this
     * runtime check into our general permission check code, so check
     * here that the reginfo's specified permissions are strict enough
     * to encompass the generic architectural permission check.
     */
    if (r->state != ARM_CP_STATE_AA32) {
        int mask = 0;
        switch (r->opc1) {
        case 0: case 1: case 2:
            /* min_EL EL1 */
            mask = PL1_RW;
            break;
        case 3:
            /* min_EL EL0 */
            mask = PL0_RW;
            break;
        case 4:
            /* min_EL EL2 */
            mask = PL2_RW;
            break;
        case 5:
            /* unallocated encoding, so not possible */
            assert(false);
            break;
        case 6:
            /* min_EL EL3 */
            mask = PL3_RW;
            break;
        case 7:
            /* min_EL EL1, secure mode only (we don't check the latter) */
            mask = PL1_RW;
            break;
        default:
            /* broken reginfo with out-of-range opc1 */
            assert(false);
            break;
        }
        /* assert our permissions are not too lax (stricter is fine) */
        assert((r->access & ~mask) == 0);
    }

    /* Check that the register definition has enough info to handle
     * reads and writes if they are permitted.
     */
    if (!(r->type & (ARM_CP_SPECIAL|ARM_CP_CONST))) {
        if (r->access & PL3_R) {
            assert((r->fieldoffset ||
                   (r->bank_fieldoffsets[0] && r->bank_fieldoffsets[1])) ||
                   r->readfn);
        }
        if (r->access & PL3_W) {
            assert((r->fieldoffset ||
                   (r->bank_fieldoffsets[0] && r->bank_fieldoffsets[1])) ||
                   r->writefn);
        }
    }
    /* Bad type field probably means missing sentinel at end of reg list */
    assert(cptype_valid(r->type));
    for (crm = crmmin; crm <= crmmax; crm++) {
        for (opc1 = opc1min; opc1 <= opc1max; opc1++) {
            for (opc2 = opc2min; opc2 <= opc2max; opc2++) {
                for (state = ARM_CP_STATE_AA32;
                     state <= ARM_CP_STATE_AA64; state++) {
                    if (r->state != state && r->state != ARM_CP_STATE_BOTH) {
                        continue;
                    }
                    if (state == ARM_CP_STATE_AA32) {
                        /* Under AArch32 CP registers can be common
                         * (same for secure and non-secure world) or banked.
                         */
                        switch (r->secure) {
                        case ARM_CP_SECSTATE_S:
                        case ARM_CP_SECSTATE_NS:
                            add_cpreg_to_hashtable(cpu, r, opaque, state,
                                                   r->secure, crm, opc1, opc2);
                            break;
                        default:
                            add_cpreg_to_hashtable(cpu, r, opaque, state,
                                                   ARM_CP_SECSTATE_S,
                                                   crm, opc1, opc2);
                            add_cpreg_to_hashtable(cpu, r, opaque, state,
                                                   ARM_CP_SECSTATE_NS,
                                                   crm, opc1, opc2);
                            break;
                        }
                    } else {
                        /* AArch64 registers get mapped to non-secure instance
                         * of AArch32 */
                        add_cpreg_to_hashtable(cpu, r, opaque, state,
                                               ARM_CP_SECSTATE_NS,
                                               crm, opc1, opc2);
                    }
                }
            }
        }
    }
}

void define_arm_cp_regs_with_opaque(ARMCPU *cpu,
                                    const ARMCPRegInfo *regs, void *opaque)
{
    /* Define a whole list of registers */
    const ARMCPRegInfo *r;
    for (r = regs; r->type != ARM_CP_SENTINEL; r++) {
        define_one_arm_cp_reg_with_opaque(cpu, r, opaque);
    }
}

const ARMCPRegInfo *get_arm_cp_reginfo(GHashTable *cpregs, uint32_t encoded_cp)
{
    return g_hash_table_lookup(cpregs, &encoded_cp);
}

void arm_cp_write_ignore(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value)
{
    /* Helper coprocessor write function for write-ignore registers */
}

uint64_t arm_cp_read_zero(CPUARMState *env, const ARMCPRegInfo *ri)
{
    /* Helper coprocessor write function for read-as-zero registers */
    return 0;
}

void arm_cp_reset_ignore(CPUARMState *env, const ARMCPRegInfo *opaque)
{
    /* Helper coprocessor reset function for do-nothing-on-reset registers */
}

static int bad_mode_switch(CPUARMState *env, int mode, CPSRWriteType write_type)
{
    /* Return true if it is not valid for us to switch to
     * this CPU mode (ie all the UNPREDICTABLE cases in
     * the ARM ARM CPSRWriteByInstr pseudocode).
     */

    /* Changes to or from Hyp via MSR and CPS are illegal. */
    if (write_type == CPSRWriteByInstr &&
        ((env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_HYP ||
         mode == ARM_CPU_MODE_HYP)) {
        return 1;
    }

    switch (mode) {
    case ARM_CPU_MODE_USR:
        return 0;
    case ARM_CPU_MODE_SYS:
    case ARM_CPU_MODE_SVC:
    case ARM_CPU_MODE_ABT:
    case ARM_CPU_MODE_UND:
    case ARM_CPU_MODE_IRQ:
    case ARM_CPU_MODE_FIQ:
        /* Note that we don't implement the IMPDEF NSACR.RFR which in v7
         * allows FIQ mode to be Secure-only. (In v8 this doesn't exist.)
         */
        /* If HCR.TGE is set then changes from Monitor to NS PL1 via MSR
         * and CPS are treated as illegal mode changes.
         */
        if (write_type == CPSRWriteByInstr &&
            (env->cp15.hcr_el2 & HCR_TGE) &&
            (env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_MON &&
            !arm_is_secure_below_el3(env)) {
            return 1;
        }
        return 0;
    case ARM_CPU_MODE_HYP:
        return !arm_feature(env, ARM_FEATURE_EL2)
            || arm_current_el(env) < 2 || arm_is_secure(env);
    case ARM_CPU_MODE_MON:
        return arm_current_el(env) < 3;
    default:
        return 1;
    }
}

uint32_t cpsr_read(CPUARMState *env)
{
    int ZF;
    ZF = (env->ZF == 0);
    return env->uncached_cpsr | (env->NF & 0x80000000) | (ZF << 30) |
        (env->CF << 29) | ((env->VF & 0x80000000) >> 3) | (env->QF << 27)
        | (env->thumb << 5) | ((env->condexec_bits & 3) << 25)
        | ((env->condexec_bits & 0xfc) << 8)
        | (env->GE << 16) | (env->daif & CPSR_AIF);
}

void cpsr_write(CPUARMState *env, uint32_t val, uint32_t mask,
                CPSRWriteType write_type)
{
    uint32_t changed_daif;

    if (mask & CPSR_NZCV) {
        env->ZF = (~val) & CPSR_Z;
        env->NF = val;
        env->CF = (val >> 29) & 1;
        env->VF = (val << 3) & 0x80000000;
    }
    if (mask & CPSR_Q)
        env->QF = ((val & CPSR_Q) != 0);
    if (mask & CPSR_T)
        env->thumb = ((val & CPSR_T) != 0);
    if (mask & CPSR_IT_0_1) {
        env->condexec_bits &= ~3;
        env->condexec_bits |= (val >> 25) & 3;
    }
    if (mask & CPSR_IT_2_7) {
        env->condexec_bits &= 3;
        env->condexec_bits |= (val >> 8) & 0xfc;
    }
    if (mask & CPSR_GE) {
        env->GE = (val >> 16) & 0xf;
    }

    /* In a V7 implementation that includes the security extensions but does
     * not include Virtualization Extensions the SCR.FW and SCR.AW bits control
     * whether non-secure software is allowed to change the CPSR_F and CPSR_A
     * bits respectively.
     *
     * In a V8 implementation, it is permitted for privileged software to
     * change the CPSR A/F bits regardless of the SCR.AW/FW bits.
     */
    if (write_type != CPSRWriteRaw && !arm_feature(env, ARM_FEATURE_V8) &&
        arm_feature(env, ARM_FEATURE_EL3) &&
        !arm_feature(env, ARM_FEATURE_EL2) &&
        !arm_is_secure(env)) {

        changed_daif = (env->daif ^ val) & mask;

        if (changed_daif & CPSR_A) {
            /* Check to see if we are allowed to change the masking of async
             * abort exceptions from a non-secure state.
             */
            if (!(env->cp15.scr_el3 & SCR_AW)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "Ignoring attempt to switch CPSR_A flag from "
                              "non-secure world with SCR.AW bit clear\n");
                mask &= ~CPSR_A;
            }
        }

        if (changed_daif & CPSR_F) {
            /* Check to see if we are allowed to change the masking of FIQ
             * exceptions from a non-secure state.
             */
            if (!(env->cp15.scr_el3 & SCR_FW)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "Ignoring attempt to switch CPSR_F flag from "
                              "non-secure world with SCR.FW bit clear\n");
                mask &= ~CPSR_F;
            }

            /* Check whether non-maskable FIQ (NMFI) support is enabled.
             * If this bit is set software is not allowed to mask
             * FIQs, but is allowed to set CPSR_F to 0.
             */
            if ((A32_BANKED_CURRENT_REG_GET(env, sctlr) & SCTLR_NMFI) &&
                (val & CPSR_F)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "Ignoring attempt to enable CPSR_F flag "
                              "(non-maskable FIQ [NMFI] support enabled)\n");
                mask &= ~CPSR_F;
            }
        }
    }

    env->daif &= ~(CPSR_AIF & mask);
    env->daif |= val & CPSR_AIF & mask;

    if (write_type != CPSRWriteRaw &&
        ((env->uncached_cpsr ^ val) & mask & CPSR_M)) {
        if ((env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_USR) {
            /* Note that we can only get here in USR mode if this is a
             * gdb stub write; for this case we follow the architectural
             * behaviour for guest writes in USR mode of ignoring an attempt
             * to switch mode. (Those are caught by translate.c for writes
             * triggered by guest instructions.)
             */
            mask &= ~CPSR_M;
        } else if (bad_mode_switch(env, val & CPSR_M, write_type)) {
            /* Attempt to switch to an invalid mode: this is UNPREDICTABLE in
             * v7, and has defined behaviour in v8:
             *  + leave CPSR.M untouched
             *  + allow changes to the other CPSR fields
             *  + set PSTATE.IL
             * For user changes via the GDB stub, we don't set PSTATE.IL,
             * as this would be unnecessarily harsh for a user error.
             */
            mask &= ~CPSR_M;
            if (write_type != CPSRWriteByGDBStub &&
                arm_feature(env, ARM_FEATURE_V8)) {
                mask |= CPSR_IL;
                val |= CPSR_IL;
            }
        } else {
            switch_mode(env, val & CPSR_M);
        }
    }
    mask &= ~CACHED_CPSR_BITS;
    env->uncached_cpsr = (env->uncached_cpsr & ~mask) | (val & mask);
}

/* Sign/zero extend */
uint32_t HELPER(sxtb16)(uint32_t x)
{
    uint32_t res;
    res = (uint16_t)(int8_t)x;
    res |= (uint32_t)(int8_t)(x >> 16) << 16;
    return res;
}

uint32_t HELPER(uxtb16)(uint32_t x)
{
    uint32_t res;
    res = (uint16_t)(uint8_t)x;
    res |= (uint32_t)(uint8_t)(x >> 16) << 16;
    return res;
}

int32_t HELPER(sdiv)(int32_t num, int32_t den)
{
    if (den == 0)
      return 0;
    if (num == INT_MIN && den == -1)
      return INT_MIN;
    return num / den;
}

uint32_t HELPER(udiv)(uint32_t num, uint32_t den)
{
    if (den == 0)
      return 0;
    return num / den;
}

uint32_t HELPER(rbit)(uint32_t x)
{
    return revbit32(x);
}

#if defined(CONFIG_USER_ONLY)

/* These should probably raise undefined insn exceptions.  */
void HELPER(v7m_msr)(CPUARMState *env, uint32_t reg, uint32_t val)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    cpu_abort(CPU(cpu), "v7m_msr %d\n", reg);
}

uint32_t HELPER(v7m_mrs)(CPUARMState *env, uint32_t reg)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    cpu_abort(CPU(cpu), "v7m_mrs %d\n", reg);
    return 0;
}

void HELPER(v7m_bxns)(CPUARMState *env, uint32_t dest)
{
    /* translate.c should never generate calls here in user-only mode */
    g_assert_not_reached();
}

void HELPER(v7m_blxns)(CPUARMState *env, uint32_t dest)
{
    /* translate.c should never generate calls here in user-only mode */
    g_assert_not_reached();
}

uint32_t HELPER(v7m_tt)(CPUARMState *env, uint32_t addr, uint32_t op)
{
    /* The TT instructions can be used by unprivileged code, but in
     * user-only emulation we don't have the MPU.
     * Luckily since we know we are NonSecure unprivileged (and that in
     * turn means that the A flag wasn't specified), all the bits in the
     * register must be zero:
     *  IREGION: 0 because IRVALID is 0
     *  IRVALID: 0 because NS
     *  S: 0 because NS
     *  NSRW: 0 because NS
     *  NSR: 0 because NS
     *  RW: 0 because unpriv and A flag not set
     *  R: 0 because unpriv and A flag not set
     *  SRVALID: 0 because NS
     *  MRVALID: 0 because unpriv and A flag not set
     *  SREGION: 0 becaus SRVALID is 0
     *  MREGION: 0 because MRVALID is 0
     */
    return 0;
}

void switch_mode(CPUARMState *env, int mode)
{
    ARMCPU *cpu = arm_env_get_cpu(env);

    if (mode != ARM_CPU_MODE_USR) {
        cpu_abort(CPU(cpu), "Tried to switch out of user mode\n");
    }
}

uint32_t arm_phys_excp_target_el(CPUState *cs, uint32_t excp_idx,
                                 uint32_t cur_el, bool secure)
{
    return 1;
}

void aarch64_sync_64_to_32(CPUARMState *env)
{
    g_assert_not_reached();
}

#else

void switch_mode(CPUARMState *env, int mode)
{
    int old_mode;
    int i;

    old_mode = env->uncached_cpsr & CPSR_M;
    if (mode == old_mode)
        return;

    if (old_mode == ARM_CPU_MODE_FIQ) {
        memcpy (env->fiq_regs, env->regs + 8, 5 * sizeof(uint32_t));
        memcpy (env->regs + 8, env->usr_regs, 5 * sizeof(uint32_t));
    } else if (mode == ARM_CPU_MODE_FIQ) {
        memcpy (env->usr_regs, env->regs + 8, 5 * sizeof(uint32_t));
        memcpy (env->regs + 8, env->fiq_regs, 5 * sizeof(uint32_t));
    }

    i = bank_number(old_mode);
    env->banked_r13[i] = env->regs[13];
    env->banked_r14[i] = env->regs[14];
    env->banked_spsr[i] = env->spsr;

    i = bank_number(mode);
    env->regs[13] = env->banked_r13[i];
    env->regs[14] = env->banked_r14[i];
    env->spsr = env->banked_spsr[i];
}

/* Physical Interrupt Target EL Lookup Table
 *
 * [ From ARM ARM section G1.13.4 (Table G1-15) ]
 *
 * The below multi-dimensional table is used for looking up the target
 * exception level given numerous condition criteria.  Specifically, the
 * target EL is based on SCR and HCR routing controls as well as the
 * currently executing EL and secure state.
 *
 *    Dimensions:
 *    target_el_table[2][2][2][2][2][4]
 *                    |  |  |  |  |  +--- Current EL
 *                    |  |  |  |  +------ Non-secure(0)/Secure(1)
 *                    |  |  |  +--------- HCR mask override
 *                    |  |  +------------ SCR exec state control
 *                    |  +--------------- SCR mask override
 *                    +------------------ 32-bit(0)/64-bit(1) EL3
 *
 *    The table values are as such:
 *    0-3 = EL0-EL3
 *     -1 = Cannot occur
 *
 * The ARM ARM target EL table includes entries indicating that an "exception
 * is not taken".  The two cases where this is applicable are:
 *    1) An exception is taken from EL3 but the SCR does not have the exception
 *    routed to EL3.
 *    2) An exception is taken from EL2 but the HCR does not have the exception
 *    routed to EL2.
 * In these two cases, the below table contain a target of EL1.  This value is
 * returned as it is expected that the consumer of the table data will check
 * for "target EL >= current EL" to ensure the exception is not taken.
 *
 *            SCR     HCR
 *         64  EA     AMO                 From
 *        BIT IRQ     IMO      Non-secure         Secure
 *        EL3 FIQ  RW FMO   EL0 EL1 EL2 EL3   EL0 EL1 EL2 EL3
 */
static const int8_t target_el_table[2][2][2][2][2][4] = {
    {{{{/* 0   0   0   0 */{ 1,  1,  2, -1 },{ 3, -1, -1,  3 },},
       {/* 0   0   0   1 */{ 2,  2,  2, -1 },{ 3, -1, -1,  3 },},},
      {{/* 0   0   1   0 */{ 1,  1,  2, -1 },{ 3, -1, -1,  3 },},
       {/* 0   0   1   1 */{ 2,  2,  2, -1 },{ 3, -1, -1,  3 },},},},
     {{{/* 0   1   0   0 */{ 3,  3,  3, -1 },{ 3, -1, -1,  3 },},
       {/* 0   1   0   1 */{ 3,  3,  3, -1 },{ 3, -1, -1,  3 },},},
      {{/* 0   1   1   0 */{ 3,  3,  3, -1 },{ 3, -1, -1,  3 },},
       {/* 0   1   1   1 */{ 3,  3,  3, -1 },{ 3, -1, -1,  3 },},},},},
    {{{{/* 1   0   0   0 */{ 1,  1,  2, -1 },{ 1,  1, -1,  1 },},
       {/* 1   0   0   1 */{ 2,  2,  2, -1 },{ 1,  1, -1,  1 },},},
      {{/* 1   0   1   0 */{ 1,  1,  1, -1 },{ 1,  1, -1,  1 },},
       {/* 1   0   1   1 */{ 2,  2,  2, -1 },{ 1,  1, -1,  1 },},},},
     {{{/* 1   1   0   0 */{ 3,  3,  3, -1 },{ 3,  3, -1,  3 },},
       {/* 1   1   0   1 */{ 3,  3,  3, -1 },{ 3,  3, -1,  3 },},},
      {{/* 1   1   1   0 */{ 3,  3,  3, -1 },{ 3,  3, -1,  3 },},
       {/* 1   1   1   1 */{ 3,  3,  3, -1 },{ 3,  3, -1,  3 },},},},},
};

/*
 * Determine the target EL for physical exceptions
 */
uint32_t arm_phys_excp_target_el(CPUState *cs, uint32_t excp_idx,
                                 uint32_t cur_el, bool secure)
{
    CPUARMState *env = cs->env_ptr;
    int rw;
    int scr;
    int hcr;
    int target_el;
    /* Is the highest EL AArch64? */
    int is64 = arm_feature(env, ARM_FEATURE_AARCH64);

    if (arm_feature(env, ARM_FEATURE_EL3)) {
        rw = ((env->cp15.scr_el3 & SCR_RW) == SCR_RW);
    } else {
        /* Either EL2 is the highest EL (and so the EL2 register width
         * is given by is64); or there is no EL2 or EL3, in which case
         * the value of 'rw' does not affect the table lookup anyway.
         */
        rw = is64;
    }

    switch (excp_idx) {
    case EXCP_IRQ:
        scr = ((env->cp15.scr_el3 & SCR_IRQ) == SCR_IRQ);
        hcr = ((env->cp15.hcr_el2 & HCR_IMO) == HCR_IMO);
        break;
    case EXCP_FIQ:
        scr = ((env->cp15.scr_el3 & SCR_FIQ) == SCR_FIQ);
        hcr = ((env->cp15.hcr_el2 & HCR_FMO) == HCR_FMO);
        break;
    default:
        scr = ((env->cp15.scr_el3 & SCR_EA) == SCR_EA);
        hcr = ((env->cp15.hcr_el2 & HCR_AMO) == HCR_AMO);
        break;
    };

    /* If HCR.TGE is set then HCR is treated as being 1 */
    hcr |= ((env->cp15.hcr_el2 & HCR_TGE) == HCR_TGE);

    /* Perform a table-lookup for the target EL given the current state */
    target_el = target_el_table[is64][scr][rw][hcr][secure][cur_el];

    assert(target_el > 0);

    return target_el;
}

static bool v7m_stack_write(ARMCPU *cpu, uint32_t addr, uint32_t value,
                            ARMMMUIdx mmu_idx, bool ignfault)
{
    CPUState *cs = CPU(cpu);
    CPUARMState *env = &cpu->env;
    MemTxAttrs attrs = {0};
    MemTxResult txres;
    target_ulong page_size;
    hwaddr physaddr;
    int prot;
    ARMMMUFaultInfo fi;
    bool secure = mmu_idx & ARM_MMU_IDX_M_S;
    int exc;
    bool exc_secure;

    if (get_phys_addr(env, addr, MMU_DATA_STORE, mmu_idx, &physaddr,
                      &attrs, &prot, &page_size, &fi, NULL)) {
        /* MPU/SAU lookup failed */
        if (fi.type == ARMFault_QEMU_SFault) {
            qemu_log_mask(CPU_LOG_INT,
                          "...SecureFault with SFSR.AUVIOL during stacking\n");
            env->v7m.sfsr |= R_V7M_SFSR_AUVIOL_MASK | R_V7M_SFSR_SFARVALID_MASK;
            env->v7m.sfar = addr;
            exc = ARMV7M_EXCP_SECURE;
            exc_secure = false;
        } else {
            qemu_log_mask(CPU_LOG_INT, "...MemManageFault with CFSR.MSTKERR\n");
            env->v7m.cfsr[secure] |= R_V7M_CFSR_MSTKERR_MASK;
            exc = ARMV7M_EXCP_MEM;
            exc_secure = secure;
        }
        goto pend_fault;
    }
    address_space_stl_le(arm_addressspace(cs, attrs), physaddr, value,
                         attrs, &txres);
    if (txres != MEMTX_OK) {
        /* BusFault trying to write the data */
        qemu_log_mask(CPU_LOG_INT, "...BusFault with BFSR.STKERR\n");
        env->v7m.cfsr[M_REG_NS] |= R_V7M_CFSR_STKERR_MASK;
        exc = ARMV7M_EXCP_BUS;
        exc_secure = false;
        goto pend_fault;
    }
    return true;

pend_fault:
    /* By pending the exception at this point we are making
     * the IMPDEF choice "overridden exceptions pended" (see the
     * MergeExcInfo() pseudocode). The other choice would be to not
     * pend them now and then make a choice about which to throw away
     * later if we have two derived exceptions.
     * The only case when we must not pend the exception but instead
     * throw it away is if we are doing the push of the callee registers
     * and we've already generated a derived exception. Even in this
     * case we will still update the fault status registers.
     */
    if (!ignfault) {
        // Unicorn: commented out
        //armv7m_nvic_set_pending_derived(env->nvic, exc, exc_secure);
    }
    return false;
}

static bool v7m_stack_read(ARMCPU *cpu, uint32_t *dest, uint32_t addr,
                           ARMMMUIdx mmu_idx)
{
    CPUState *cs = CPU(cpu);
    CPUARMState *env = &cpu->env;
    MemTxAttrs attrs = {};
    MemTxResult txres;
    target_ulong page_size;
    hwaddr physaddr;
    int prot;
    ARMMMUFaultInfo fi;
    bool secure = mmu_idx & ARM_MMU_IDX_M_S;
    int exc;
    bool exc_secure;
    uint32_t value;

    if (get_phys_addr(env, addr, MMU_DATA_LOAD, mmu_idx, &physaddr,
                      &attrs, &prot, &page_size, &fi, NULL)) {
        /* MPU/SAU lookup failed */
        if (fi.type == ARMFault_QEMU_SFault) {
            qemu_log_mask(CPU_LOG_INT,
                          "...SecureFault with SFSR.AUVIOL during unstack\n");
            env->v7m.sfsr |= R_V7M_SFSR_AUVIOL_MASK | R_V7M_SFSR_SFARVALID_MASK;
            env->v7m.sfar = addr;
            exc = ARMV7M_EXCP_SECURE;
            exc_secure = false;
        } else {
            qemu_log_mask(CPU_LOG_INT,
                          "...MemManageFault with CFSR.MUNSTKERR\n");
            env->v7m.cfsr[secure] |= R_V7M_CFSR_MUNSTKERR_MASK;
            exc = ARMV7M_EXCP_MEM;
            exc_secure = secure;
        }
        goto pend_fault;
    }

    value = address_space_ldl(arm_addressspace(cs, attrs), physaddr,
                              attrs, &txres);
    if (txres != MEMTX_OK) {
        /* BusFault trying to read the data */
        qemu_log_mask(CPU_LOG_INT, "...BusFault with BFSR.UNSTKERR\n");
        env->v7m.cfsr[M_REG_NS] |= R_V7M_CFSR_UNSTKERR_MASK;
        exc = ARMV7M_EXCP_BUS;
        exc_secure = false;
        goto pend_fault;
    }

    *dest = value;
    return true;

pend_fault:
    /* By pending the exception at this point we are making
     * the IMPDEF choice "overridden exceptions pended" (see the
     * MergeExcInfo() pseudocode). The other choice would be to not
     * pend them now and then make a choice about which to throw away
     * later if we have two derived exceptions.
     */
    // Unicorn: commented out
    //armv7m_nvic_set_pending(env->nvic, exc, exc_secure);
    return false;
}

/* Return true if we're using the process stack pointer (not the MSP) */
static bool v7m_using_psp(CPUARMState *env)
{
    /* Handler mode always uses the main stack; for thread mode
     * the CONTROL.SPSEL bit determines the answer.
     * Note that in v7M it is not possible to be in Handler mode with
     * CONTROL.SPSEL non-zero, but in v8M it is, so we must check both.
     */
    return !arm_v7m_is_handler_mode(env) &&
        env->v7m.control[env->v7m.secure] & R_V7M_CONTROL_SPSEL_MASK;
}

/* Write to v7M CONTROL.SPSEL bit for the specified security bank.
 * This may change the current stack pointer between Main and Process
 * stack pointers if it is done for the CONTROL register for the current
 * security state.
 */
static void write_v7m_control_spsel_for_secstate(CPUARMState *env,
                                                 bool new_spsel,
                                                 bool secstate)
{
    bool old_is_psp = v7m_using_psp(env);

    env->v7m.control[secstate] =
        deposit32(env->v7m.control[secstate],
                  R_V7M_CONTROL_SPSEL_SHIFT,
                  R_V7M_CONTROL_SPSEL_LENGTH, new_spsel);

    if (secstate == env->v7m.secure) {
        bool new_is_psp = v7m_using_psp(env);
        uint32_t tmp;

        if (old_is_psp != new_is_psp) {
            tmp = env->v7m.other_sp;
            env->v7m.other_sp = env->regs[13];
            env->regs[13] = tmp;
        }
    }
}

/* Write to v7M CONTROL.SPSEL bit. This may change the current
 * stack pointer between Main and Process stack pointers.
 */
static void write_v7m_control_spsel(CPUARMState *env, bool new_spsel)
{
    write_v7m_control_spsel_for_secstate(env, new_spsel, env->v7m.secure);
}

void write_v7m_exception(CPUARMState *env, uint32_t new_exc)
{
    /* Write a new value to v7m.exception, thus transitioning into or out
     * of Handler mode; this may result in a change of active stack pointer.
     */
    bool new_is_psp, old_is_psp = v7m_using_psp(env);
    uint32_t tmp;

    env->v7m.exception = new_exc;

    new_is_psp = v7m_using_psp(env);

    if (old_is_psp != new_is_psp) {
        tmp = env->v7m.other_sp;
        env->v7m.other_sp = env->regs[13];
        env->regs[13] = tmp;
    }
}

/* Switch M profile security state between NS and S */
static void switch_v7m_security_state(CPUARMState *env, bool new_secstate)
{
    uint32_t new_ss_msp, new_ss_psp;

    if (env->v7m.secure == new_secstate) {
        return;
    }

    /* All the banked state is accessed by looking at env->v7m.secure
     * except for the stack pointer; rearrange the SP appropriately.
     */
    new_ss_msp = env->v7m.other_ss_msp;
    new_ss_psp = env->v7m.other_ss_psp;

    if (v7m_using_psp(env)) {
        env->v7m.other_ss_psp = env->regs[13];
        env->v7m.other_ss_msp = env->v7m.other_sp;
    } else {
        env->v7m.other_ss_msp = env->regs[13];
        env->v7m.other_ss_psp = env->v7m.other_sp;
    }

    env->v7m.secure = new_secstate;

    if (v7m_using_psp(env)) {
        env->regs[13] = new_ss_psp;
        env->v7m.other_sp = new_ss_msp;
    } else {
        env->regs[13] = new_ss_msp;
        env->v7m.other_sp = new_ss_psp;
    }
}

void HELPER(v7m_bxns)(CPUARMState *env, uint32_t dest)
{
    /* Handle v7M BXNS:
     *  - if the return value is a magic value, do exception return (like BX)
     *  - otherwise bit 0 of the return value is the target security state
     */
    uint32_t min_magic;

    if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
        /* Covers FNC_RETURN and EXC_RETURN magic */
        min_magic = FNC_RETURN_MIN_MAGIC;
    } else {
        /* EXC_RETURN magic only */
        min_magic = EXC_RETURN_MIN_MAGIC;
    }

    if (dest >= min_magic) {
        /* This is an exception return magic value; put it where
         * do_v7m_exception_exit() expects and raise EXCEPTION_EXIT.
         * Note that if we ever add gen_ss_advance() singlestep support to
         * M profile this should count as an "instruction execution complete"
         * event (compare gen_bx_excret_final_code()).
         */
        env->regs[15] = dest & ~1;
        env->thumb = dest & 1;
        HELPER(exception_internal)(env, EXCP_EXCEPTION_EXIT);
        /* notreached */
    }

    /* translate.c should have made BXNS UNDEF unless we're secure */
    assert(env->v7m.secure);

    switch_v7m_security_state(env, dest & 1);
    env->thumb = 1;
    env->regs[15] = dest & ~1;
}

void HELPER(v7m_blxns)(CPUARMState *env, uint32_t dest)
{
    /* Handle v7M BLXNS:
     *  - bit 0 of the destination address is the target security state
     */

    /* At this point regs[15] is the address just after the BLXNS */
    uint32_t nextinst = env->regs[15] | 1;
    uint32_t sp = env->regs[13] - 8;
    uint32_t saved_psr;

    /* translate.c will have made BLXNS UNDEF unless we're secure */
    assert(env->v7m.secure);

    if (dest & 1) {
        /* target is Secure, so this is just a normal BLX,
         * except that the low bit doesn't indicate Thumb/not.
         */
        env->regs[14] = nextinst;
        env->thumb = 1;
        env->regs[15] = dest & ~1;
        return;
    }

    /* Target is non-secure: first push a stack frame */
    if (!QEMU_IS_ALIGNED(sp, 8)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "BLXNS with misaligned SP is UNPREDICTABLE\n");
    }

    saved_psr = env->v7m.exception;
    if (env->v7m.control[M_REG_S] & R_V7M_CONTROL_SFPA_MASK) {
        saved_psr |= XPSR_SFPA;
    }

    /* Note that these stores can throw exceptions on MPU faults */
    cpu_stl_data(env, sp, nextinst);
    cpu_stl_data(env, sp + 4, saved_psr);

    env->regs[13] = sp;
    env->regs[14] = 0xfeffffff;
    if (arm_v7m_is_handler_mode(env)) {
        /* Write a dummy value to IPSR, to avoid leaking the current secure
         * exception number to non-secure code. This is guaranteed not
         * to cause write_v7m_exception() to actually change stacks.
         */
        write_v7m_exception(env, 1);
    }
    switch_v7m_security_state(env, 0);
    env->thumb = 1;
    env->regs[15] = dest;
}

static uint32_t *get_v7m_sp_ptr(CPUARMState *env, bool secure, bool threadmode,
                                bool spsel)
{
    /* Return a pointer to the location where we currently store the
     * stack pointer for the requested security state and thread mode.
     * This pointer will become invalid if the CPU state is updated
     * such that the stack pointers are switched around (eg changing
     * the SPSEL control bit).
     * Compare the v8M ARM ARM pseudocode LookUpSP_with_security_mode().
     * Unlike that pseudocode, we require the caller to pass us in the
     * SPSEL control bit value; this is because we also use this
     * function in handling of pushing of the callee-saves registers
     * part of the v8M stack frame (pseudocode PushCalleeStack()),
     * and in the tailchain codepath the SPSEL bit comes from the exception
     * return magic LR value from the previous exception. The pseudocode
     * opencodes the stack-selection in PushCalleeStack(), but we prefer
     * to make this utility function generic enough to do the job.
     */
    bool want_psp = threadmode && spsel;

    if (secure == env->v7m.secure) {
        if (want_psp == v7m_using_psp(env)) {
            return &env->regs[13];
        } else {
            return &env->v7m.other_sp;
        }
    } else {
        if (want_psp) {
            return &env->v7m.other_ss_psp;
        } else {
            return &env->v7m.other_ss_msp;
        }
    }
}

static bool arm_v7m_load_vector(ARMCPU *cpu, int exc, bool targets_secure,
                                uint32_t *pvec)
{
    CPUState *cs = CPU(cpu);
    CPUARMState *env = &cpu->env;
    MemTxResult result;
    uint32_t addr = env->v7m.vecbase[targets_secure] + exc * 4;
    uint32_t vector_entry;
    MemTxAttrs attrs = {0};
    ARMMMUIdx mmu_idx;
    bool exc_secure;

    mmu_idx = arm_v7m_mmu_idx_for_secstate_and_priv(env, targets_secure, true);

    /* We don't do a get_phys_addr() here because the rules for vector
     * loads are special: they always use the default memory map, and
     * the default memory map permits reads from all addresses.
     * Since there's no easy way to pass through to pmsav8_mpu_lookup()
     * that we want this special case which would always say "yes",
     * we just do the SAU lookup here followed by a direct physical load.
     */
    attrs.secure = targets_secure;
    attrs.user = false;

    if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
        V8M_SAttributes sattrs = {0};

        v8m_security_lookup(env, addr, MMU_DATA_LOAD, mmu_idx, &sattrs);
        if (sattrs.ns) {
            attrs.secure = false;
        } else if (!targets_secure) {
            /* NS access to S memory */
            goto load_fail;
        }
    }

    vector_entry = address_space_ldl(arm_addressspace(cs, attrs), addr,
                                     attrs, &result);
    if (result != MEMTX_OK) {
        goto load_fail;
    }
    *pvec = vector_entry;
    return true;

load_fail:
    /* All vector table fetch fails are reported as HardFault, with
     * HFSR.VECTTBL and .FORCED set. (FORCED is set because
     * technically the underlying exception is a MemManage or BusFault
     * that is escalated to HardFault.) This is a terminal exception,
     * so we will either take the HardFault immediately or else enter
     * lockup (the latter case is handled in armv7m_nvic_set_pending_derived()).
     */
    exc_secure = targets_secure ||
        !(cpu->env.v7m.aircr & R_V7M_AIRCR_BFHFNMINS_MASK);
    env->v7m.hfsr |= R_V7M_HFSR_VECTTBL_MASK | R_V7M_HFSR_FORCED_MASK;
    // Unicorn: commented out
    //armv7m_nvic_set_pending_derived(env->nvic, ARMV7M_EXCP_HARD, exc_secure);
    return false;
}

static bool v7m_push_callee_stack(ARMCPU *cpu, uint32_t lr, bool dotailchain,
                                  bool ignore_faults)
{
    /* For v8M, push the callee-saves register part of the stack frame.
     * Compare the v8M pseudocode PushCalleeStack().
     * In the tailchaining case this may not be the current stack.
     */
    CPUARMState *env = &cpu->env;
    uint32_t *frame_sp_p;
    uint32_t frameptr;
    ARMMMUIdx mmu_idx;
    bool stacked_ok;

    if (dotailchain) {
        bool mode = lr & R_V7M_EXCRET_MODE_MASK;
        bool priv = !(env->v7m.control[M_REG_S] & R_V7M_CONTROL_NPRIV_MASK) ||
            !mode;

        mmu_idx = arm_v7m_mmu_idx_for_secstate_and_priv(env, M_REG_S, priv);
        frame_sp_p = get_v7m_sp_ptr(env, M_REG_S, mode,
                                    lr & R_V7M_EXCRET_SPSEL_MASK);
    } else {
        mmu_idx = core_to_arm_mmu_idx(env, cpu_mmu_index(env, false));
        frame_sp_p = &env->regs[13];
    }

    frameptr = *frame_sp_p - 0x28;

    /* Write as much of the stack frame as we can. A write failure may
     * cause us to pend a derived exception.
     */
    stacked_ok =
        v7m_stack_write(cpu, frameptr, 0xfefa125b, mmu_idx, ignore_faults) &&
        v7m_stack_write(cpu, frameptr + 0x8, env->regs[4], mmu_idx,
                        ignore_faults) &&
        v7m_stack_write(cpu, frameptr + 0xc, env->regs[5], mmu_idx,
                        ignore_faults) &&
        v7m_stack_write(cpu, frameptr + 0x10, env->regs[6], mmu_idx,
                        ignore_faults) &&
        v7m_stack_write(cpu, frameptr + 0x14, env->regs[7], mmu_idx,
                        ignore_faults) &&
        v7m_stack_write(cpu, frameptr + 0x18, env->regs[8], mmu_idx,
                        ignore_faults) &&
        v7m_stack_write(cpu, frameptr + 0x1c, env->regs[9], mmu_idx,
                        ignore_faults) &&
        v7m_stack_write(cpu, frameptr + 0x20, env->regs[10], mmu_idx,
                        ignore_faults) &&
        v7m_stack_write(cpu, frameptr + 0x24, env->regs[11], mmu_idx,
                        ignore_faults);

    /* Update SP regardless of whether any of the stack accesses failed.
     * When we implement v8M stack limit checking then this attempt to
     * update SP might also fail and result in a derived exception.
     */
    *frame_sp_p = frameptr;

    return !stacked_ok;
}

static void v7m_exception_taken(ARMCPU *cpu, uint32_t lr, bool dotailchain,
                                bool ignore_stackfaults)
{
    /* Do the "take the exception" parts of exception entry,
     * but not the pushing of state to the stack. This is
     * similar to the pseudocode ExceptionTaken() function.
     */
    CPUARMState *env = &cpu->env;
    uint32_t addr;
    bool targets_secure = false;
    int exc = 0;
    bool push_failed = false;

    // Unicorn: commented out
    //armv7m_nvic_get_pending_irq_info(env->nvic, &exc, &targets_secure);

    if (arm_feature(env, ARM_FEATURE_V8)) {
        if (arm_feature(env, ARM_FEATURE_M_SECURITY) &&
            (lr & R_V7M_EXCRET_S_MASK)) {
            /* The background code (the owner of the registers in the
             * exception frame) is Secure. This means it may either already
             * have or now needs to push callee-saves registers.
             */
            if (targets_secure) {
                if (dotailchain && !(lr & R_V7M_EXCRET_ES_MASK)) {
                    /* We took an exception from Secure to NonSecure
                     * (which means the callee-saved registers got stacked)
                     * and are now tailchaining to a Secure exception.
                     * Clear DCRS so eventual return from this Secure
                     * exception unstacks the callee-saved registers.
                     */
                    lr &= ~R_V7M_EXCRET_DCRS_MASK;
                }
            } else {
                /* We're going to a non-secure exception; push the
                 * callee-saves registers to the stack now, if they're
                 * not already saved.
                 */
                if (lr & R_V7M_EXCRET_DCRS_MASK &&
                    !(dotailchain && (lr & R_V7M_EXCRET_ES_MASK))) {
                    push_failed = v7m_push_callee_stack(cpu, lr, dotailchain,
                                                        ignore_stackfaults);
                }
                lr |= R_V7M_EXCRET_DCRS_MASK;
            }
        }

        lr &= ~R_V7M_EXCRET_ES_MASK;
        if (targets_secure || !arm_feature(env, ARM_FEATURE_M_SECURITY)) {
            lr |= R_V7M_EXCRET_ES_MASK;
        }
        lr &= ~R_V7M_EXCRET_SPSEL_MASK;
        if (env->v7m.control[targets_secure] & R_V7M_CONTROL_SPSEL_MASK) {
            lr |= R_V7M_EXCRET_SPSEL_MASK;
        }

        /* Clear registers if necessary to prevent non-secure exception
         * code being able to see register values from secure code.
         * Where register values become architecturally UNKNOWN we leave
         * them with their previous values.
         */
        if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
            if (!targets_secure) {
                /* Always clear the caller-saved registers (they have been
                 * pushed to the stack earlier in v7m_push_stack()).
                 * Clear callee-saved registers if the background code is
                 * Secure (in which case these regs were saved in
                 * v7m_push_callee_stack()).
                 */
                int i;

                for (i = 0; i < 13; i++) {
                    /* r4..r11 are callee-saves, zero only if EXCRET.S == 1 */
                    if (i < 4 || i > 11 || (lr & R_V7M_EXCRET_S_MASK)) {
                        env->regs[i] = 0;
                    }
                }
                /* Clear EAPSR */
                xpsr_write(env, 0, XPSR_NZCV | XPSR_Q | XPSR_GE | XPSR_IT);
            }
        }
    }

    if (push_failed && !ignore_stackfaults) {
        /* Derived exception on callee-saves register stacking:
         * we might now want to take a different exception which
         * targets a different security state, so try again from the top.
         */
        v7m_exception_taken(cpu, lr, true, true);
        return;
    }

    if (!arm_v7m_load_vector(cpu, exc, targets_secure, &addr)) {
        /* Vector load failed: derived exception */
        v7m_exception_taken(cpu, lr, true, true);
        return;
    }

    /* Now we've done everything that might cause a derived exception
     * we can go ahead and activate whichever exception we're going to
     * take (which might now be the derived exception).
     */
    // Unicorn: commented out
    //armv7m_nvic_acknowledge_irq(env->nvic);

    /* Switch to target security state -- must do this before writing SPSEL */
    switch_v7m_security_state(env, targets_secure);
    write_v7m_control_spsel(env, 0);
    arm_clear_exclusive(env);
    /* Clear IT bits */
    env->condexec_bits = 0;
    env->regs[14] = lr;
    env->regs[15] = addr & 0xfffffffe;
    env->thumb = addr & 1;
}

static bool v7m_push_stack(ARMCPU *cpu)
{
    /* Do the "set up stack frame" part of exception entry,
     * similar to pseudocode PushStack().
     * Return true if we generate a derived exception (and so
     * should ignore further stack faults trying to process
     * that derived exception.)
     */
    bool stacked_ok;
    CPUARMState *env = &cpu->env;
    uint32_t xpsr = xpsr_read(env);
    uint32_t frameptr = env->regs[13];
    ARMMMUIdx mmu_idx = core_to_arm_mmu_idx(env, cpu_mmu_index(env, false));

    /* Align stack pointer if the guest wants that */
    if ((frameptr & 4) &&
        (env->v7m.ccr[env->v7m.secure] & R_V7M_CCR_STKALIGN_MASK)) {
        frameptr -= 4;
        xpsr |= XPSR_SPREALIGN;
    }

    frameptr -= 0x20;

    /* Write as much of the stack frame as we can. If we fail a stack
     * write this will result in a derived exception being pended
     * (which may be taken in preference to the one we started with
     * if it has higher priority).
     */
    stacked_ok =
        v7m_stack_write(cpu, frameptr, env->regs[0], mmu_idx, false) &&
        v7m_stack_write(cpu, frameptr + 4, env->regs[1], mmu_idx, false) &&
        v7m_stack_write(cpu, frameptr + 8, env->regs[2], mmu_idx, false) &&
        v7m_stack_write(cpu, frameptr + 12, env->regs[3], mmu_idx, false) &&
        v7m_stack_write(cpu, frameptr + 16, env->regs[12], mmu_idx, false) &&
        v7m_stack_write(cpu, frameptr + 20, env->regs[14], mmu_idx, false) &&
        v7m_stack_write(cpu, frameptr + 24, env->regs[15], mmu_idx, false) &&
        v7m_stack_write(cpu, frameptr + 28, xpsr, mmu_idx, false);

    /* Update SP regardless of whether any of the stack accesses failed.
     * When we implement v8M stack limit checking then this attempt to
     * update SP might also fail and result in a derived exception.
     */
    env->regs[13] = frameptr;

    return !stacked_ok;
}

static void do_v7m_exception_exit(ARMCPU *cpu)
{
    CPUARMState *env = &cpu->env;
    CPUState *cs = CPU(cpu);
    uint32_t excret;
    uint32_t xpsr;
    bool ufault = false;
    bool sfault = false;
    bool return_to_sp_process;
    bool return_to_handler;
    bool rettobase = false;
    bool exc_secure = false;
    bool return_to_secure;

    /* If we're not in Handler mode then jumps to magic exception-exit
     * addresses don't have magic behaviour. However for the v8M
     * security extensions the magic secure-function-return has to
     * work in thread mode too, so to avoid doing an extra check in
     * the generated code we allow exception-exit magic to also cause the
     * internal exception and bring us here in thread mode. Correct code
     * will never try to do this (the following insn fetch will always
     * fault) so we the overhead of having taken an unnecessary exception
     * doesn't matter.
     */
    if (!arm_v7m_is_handler_mode(env)) {
        return;
    }

    /* In the spec pseudocode ExceptionReturn() is called directly
     * from BXWritePC() and gets the full target PC value including
     * bit zero. In QEMU's implementation we treat it as a normal
     * jump-to-register (which is then caught later on), and so split
     * the target value up between env->regs[15] and env->thumb in
     * gen_bx(). Reconstitute it.
     */
    excret = env->regs[15];
    if (env->thumb) {
        excret |= 1;
    }

    qemu_log_mask(CPU_LOG_INT, "Exception return: magic PC %" PRIx32
                  " previous exception %d\n",
                  excret, env->v7m.exception);

    if ((excret & R_V7M_EXCRET_RES1_MASK) != R_V7M_EXCRET_RES1_MASK) {
        qemu_log_mask(LOG_GUEST_ERROR, "M profile: zero high bits in exception "
                      "exit PC value 0x%" PRIx32 " are UNPREDICTABLE\n",
                      excret);
    }

    if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
        /* EXC_RETURN.ES validation check (R_SMFL). We must do this before
         * we pick which FAULTMASK to clear.
         */
        if (!env->v7m.secure &&
            ((excret & R_V7M_EXCRET_ES_MASK) ||
             !(excret & R_V7M_EXCRET_DCRS_MASK))) {
            sfault = 1;
            /* For all other purposes, treat ES as 0 (R_HXSR) */
            excret &= ~R_V7M_EXCRET_ES_MASK;
        }
    }

    if (env->v7m.exception != ARMV7M_EXCP_NMI) {
        /* Auto-clear FAULTMASK on return from other than NMI.
         * If the security extension is implemented then this only
         * happens if the raw execution priority is >= 0; the
         * value of the ES bit in the exception return value indicates
         * which security state's faultmask to clear. (v8M ARM ARM R_KBNF.)
         */
        if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
            exc_secure = excret & R_V7M_EXCRET_ES_MASK;
            // Unicorn: commented out
            //if (armv7m_nvic_raw_execution_priority(env->nvic) >= 0) {
            //    env->v7m.faultmask[es] = 0;
            //}
        } else {
            env->v7m.faultmask[M_REG_NS] = 0;
        }
    }

// Unicorn: if'd out
#if 0
    switch (armv7m_nvic_complete_irq(env->nvic, env->v7m.exception)) {
    case -1:
        /* attempt to exit an exception that isn't active */
        ufault = true;
        break;
    case 0:
        /* still an irq active now */
        break;
    case 1:
        /* we returned to base exception level, no nesting.
         * (In the pseudocode this is written using "NestedActivation != 1"
         * where we have 'rettobase == false'.)
         */
        rettobase = true;
        break;
    default:
        g_assert_not_reached();
    }
#endif

    return_to_handler = !(excret & R_V7M_EXCRET_MODE_MASK);
    return_to_sp_process = excret & R_V7M_EXCRET_SPSEL_MASK;
    return_to_secure = arm_feature(env, ARM_FEATURE_M_SECURITY) &&
        (excret & R_V7M_EXCRET_S_MASK);

    if (arm_feature(env, ARM_FEATURE_V8)) {
        if (!arm_feature(env, ARM_FEATURE_M_SECURITY)) {
            /* UNPREDICTABLE if S == 1 or DCRS == 0 or ES == 1 (R_XLCP);
             * we choose to take the UsageFault.
             */
            if ((excret & R_V7M_EXCRET_S_MASK) ||
                (excret & R_V7M_EXCRET_ES_MASK) ||
                !(excret & R_V7M_EXCRET_DCRS_MASK)) {
                ufault = true;
            }
        }
        if (excret & R_V7M_EXCRET_RES0_MASK) {
            ufault = true;
        }
    } else {
        /* For v7M we only recognize certain combinations of the low bits */
        switch (excret & 0xf) {
        case 1: /* Return to Handler */
            break;
        case 13: /* Return to Thread using Process stack */
        case 9: /* Return to Thread using Main stack */
            /* We only need to check NONBASETHRDENA for v7M, because in
             * v8M this bit does not exist (it is RES1).
             */
            if (!rettobase &&
                !(env->v7m.ccr[env->v7m.secure] &
                  R_V7M_CCR_NONBASETHRDENA_MASK)) {
                ufault = true;
            }
            break;
        default:
            ufault = true;
        }
    }

    if (sfault) {
        env->v7m.sfsr |= R_V7M_SFSR_INVER_MASK;
        // Unicorn: commented out
        //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_SECURE, false);
        v7m_exception_taken(cpu, excret, true, false);
        qemu_log_mask(CPU_LOG_INT, "...taking SecureFault on existing "
                      "stackframe: failed EXC_RETURN.ES validity check\n");
        return;
    }

    if (ufault) {
        /* Bad exception return: instead of popping the exception
         * stack, directly take a usage fault on the current stack.
         */
        env->v7m.cfsr[env->v7m.secure] |= R_V7M_CFSR_INVPC_MASK;
        // Unicorn: commented out
        //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_USAGE);
        v7m_exception_taken(cpu, excret, true, false);
        qemu_log_mask(CPU_LOG_INT, "...taking UsageFault on existing "
                      "stackframe: failed exception return integrity check\n");
        return;
    }

    /* Set CONTROL.SPSEL from excret.SPSEL. Since we're still in
     * Handler mode (and will be until we write the new XPSR.Interrupt
     * field) this does not switch around the current stack pointer.
     */
    write_v7m_control_spsel_for_secstate(env, return_to_sp_process, exc_secure);

    switch_v7m_security_state(env, return_to_secure);

    {
        /* The stack pointer we should be reading the exception frame from
         * depends on bits in the magic exception return type value (and
         * for v8M isn't necessarily the stack pointer we will eventually
         * end up resuming execution with). Get a pointer to the location
         * in the CPU state struct where the SP we need is currently being
         * stored; we will use and modify it in place.
         * We use this limited C variable scope so we don't accidentally
         * use 'frame_sp_p' after we do something that makes it invalid.
         */
        uint32_t *frame_sp_p = get_v7m_sp_ptr(env,
                                              return_to_secure,
                                              !return_to_handler,
                                              return_to_sp_process);
        uint32_t frameptr = *frame_sp_p;
        bool pop_ok = true;
        ARMMMUIdx mmu_idx;

        mmu_idx = arm_v7m_mmu_idx_for_secstate_and_priv(env, return_to_secure,
                                                        !return_to_handler);

        if (!QEMU_IS_ALIGNED(frameptr, 8) &&
            arm_feature(env, ARM_FEATURE_V8)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "M profile exception return with non-8-aligned SP "
                          "for destination state is UNPREDICTABLE\n");
        }

        /* Do we need to pop callee-saved registers? */
        if (return_to_secure &&
            ((excret & R_V7M_EXCRET_ES_MASK) == 0 ||
             (excret & R_V7M_EXCRET_DCRS_MASK) == 0)) {
            uint32_t expected_sig = 0xfefa125b;
            uint32_t actual_sig = ldl_phys(cs->as, frameptr);

            if (expected_sig != actual_sig) {
                /* Take a SecureFault on the current stack */
                env->v7m.sfsr |= R_V7M_SFSR_INVIS_MASK;
                // Unicorn: commented out
                //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_SECURE, false);
                v7m_exception_taken(cpu, excret, true, false);
                qemu_log_mask(CPU_LOG_INT, "...taking SecureFault on existing "
                              "stackframe: failed exception return integrity "
                              "signature check\n");
                return;
            }

            pop_ok =
                v7m_stack_read(cpu, &env->regs[4], frameptr + 0x8, mmu_idx) &&
                v7m_stack_read(cpu, &env->regs[4], frameptr + 0x8, mmu_idx) &&
                v7m_stack_read(cpu, &env->regs[5], frameptr + 0xc, mmu_idx) &&
                v7m_stack_read(cpu, &env->regs[6], frameptr + 0x10, mmu_idx) &&
                v7m_stack_read(cpu, &env->regs[7], frameptr + 0x14, mmu_idx) &&
                v7m_stack_read(cpu, &env->regs[8], frameptr + 0x18, mmu_idx) &&
                v7m_stack_read(cpu, &env->regs[9], frameptr + 0x1c, mmu_idx) &&
                v7m_stack_read(cpu, &env->regs[10], frameptr + 0x20, mmu_idx) &&
                v7m_stack_read(cpu, &env->regs[11], frameptr + 0x24, mmu_idx);

            frameptr += 0x28;
        }

        /* Pop registers */
        pop_ok = pop_ok &&
            v7m_stack_read(cpu, &env->regs[0], frameptr, mmu_idx) &&
            v7m_stack_read(cpu, &env->regs[1], frameptr + 0x4, mmu_idx) &&
            v7m_stack_read(cpu, &env->regs[2], frameptr + 0x8, mmu_idx) &&
            v7m_stack_read(cpu, &env->regs[3], frameptr + 0xc, mmu_idx) &&
            v7m_stack_read(cpu, &env->regs[12], frameptr + 0x10, mmu_idx) &&
            v7m_stack_read(cpu, &env->regs[14], frameptr + 0x14, mmu_idx) &&
            v7m_stack_read(cpu, &env->regs[15], frameptr + 0x18, mmu_idx) &&
            v7m_stack_read(cpu, &xpsr, frameptr + 0x1c, mmu_idx);

        if (!pop_ok) {
            /* v7m_stack_read() pended a fault, so take it (as a tail
             * chained exception on the same stack frame)
             */
            v7m_exception_taken(cpu, excret, true, false);
            return;
        }

        /* Returning from an exception with a PC with bit 0 set is defined
         * behaviour on v8M (bit 0 is ignored), but for v7M it was specified
         * to be UNPREDICTABLE. In practice actual v7M hardware seems to ignore
         * the lsbit, and there are several RTOSes out there which incorrectly
         * assume the r15 in the stack frame should be a Thumb-style "lsbit
         * indicates ARM/Thumb" value, so ignore the bit on v7M as well, but
         * complain about the badly behaved guest.
         */
        if (env->regs[15] & 1) {
            env->regs[15] &= ~1U;
            if (!arm_feature(env, ARM_FEATURE_V8)) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "M profile return from interrupt with misaligned "
                              "PC is UNPREDICTABLE on v7M\n");
            }
        }

        if (arm_feature(env, ARM_FEATURE_V8)) {
            /* For v8M we have to check whether the xPSR exception field
             * matches the EXCRET value for return to handler/thread
             * before we commit to changing the SP and xPSR.
             */
            bool will_be_handler = (xpsr & XPSR_EXCP) != 0;
            if (return_to_handler != will_be_handler) {
                /* Take an INVPC UsageFault on the current stack.
                 * By this point we will have switched to the security state
                 * for the background state, so this UsageFault will target
                 * that state.
                 */
                // Unicorn: commented out
                //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_USAGE,
                //                        env->v7m.secure);
                env->v7m.cfsr[env->v7m.secure] |= R_V7M_CFSR_INVPC_MASK;
                v7m_exception_taken(cpu, excret, true, false);
                qemu_log_mask(CPU_LOG_INT, "...taking UsageFault on existing "
                              "stackframe: failed exception return integrity "
                              "check\n");
                return;
            }
        }

        /* Commit to consuming the stack frame */
        frameptr += 0x20;
        /* Undo stack alignment (the SPREALIGN bit indicates that the original
         * pre-exception SP was not 8-aligned and we added a padding word to
         * align it, so we undo this by ORing in the bit that increases it
         * from the current 8-aligned value to the 8-unaligned value. (Adding 4
         * would work too but a logical OR is how the pseudocode specifies it.)
         */
        if (xpsr & XPSR_SPREALIGN) {
            frameptr |= 4;
        }
        *frame_sp_p = frameptr;
    }
    /* This xpsr_write() will invalidate frame_sp_p as it may switch stack */
    xpsr_write(env, xpsr, ~XPSR_SPREALIGN);

    /* The restored xPSR exception field will be zero if we're
     * resuming in Thread mode. If that doesn't match what the
     * exception return excret specified then this is a UsageFault.
     * v7M requires we make this check here; v8M did it earlier.
     */
    if (return_to_handler != arm_v7m_is_handler_mode(env)) {
        /* Take an INVPC UsageFault by pushing the stack again;
         * we know we're v7M so this is never a Secure UsageFault.
         */
        bool ignore_stackfaults;

        assert(!arm_feature(env, ARM_FEATURE_V8));
        // Unicorn: commented out
        //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_USAGE);
        env->v7m.cfsr[env->v7m.secure] |= R_V7M_CFSR_INVPC_MASK;
        ignore_stackfaults = v7m_push_stack(cpu);
        v7m_exception_taken(cpu, excret, false, ignore_stackfaults);
        qemu_log_mask(CPU_LOG_INT, "...taking UsageFault on new stackframe: "
                      "failed exception return integrity check\n");
        return;
    }

    /* Otherwise, we have a successful exception exit. */
    arm_clear_exclusive(env);
    qemu_log_mask(CPU_LOG_INT, "...successful exception return\n");
}

static bool do_v7m_function_return(ARMCPU *cpu)
{
    /* v8M security extensions magic function return.
     * We may either:
     *  (1) throw an exception (longjump)
     *  (2) return true if we successfully handled the function return
     *  (3) return false if we failed a consistency check and have
     *      pended a UsageFault that needs to be taken now
     *
     * At this point the magic return value is split between env->regs[15]
     * and env->thumb. We don't bother to reconstitute it because we don't
     * need it (all values are handled the same way).
     */
    CPUARMState *env = &cpu->env;
    uint32_t newpc, newpsr, newpsr_exc;

    qemu_log_mask(CPU_LOG_INT, "...really v7M secure function return\n");

    {
        bool threadmode, spsel;
        TCGMemOpIdx oi;
        ARMMMUIdx mmu_idx;
        uint32_t *frame_sp_p;
        uint32_t frameptr;

        /* Pull the return address and IPSR from the Secure stack */
        threadmode = !arm_v7m_is_handler_mode(env);
        spsel = env->v7m.control[M_REG_S] & R_V7M_CONTROL_SPSEL_MASK;

        frame_sp_p = get_v7m_sp_ptr(env, true, threadmode, spsel);
        frameptr = *frame_sp_p;

        /* These loads may throw an exception (for MPU faults). We want to
         * do them as secure, so work out what MMU index that is.
         */
        mmu_idx = arm_v7m_mmu_idx_for_secstate(env, true);
        oi = make_memop_idx(MO_LE, arm_to_core_mmu_idx(mmu_idx));
        newpc = helper_le_ldul_mmu(env, frameptr, oi, 0);
        newpsr = helper_le_ldul_mmu(env, frameptr + 4, oi, 0);

        /* Consistency checks on new IPSR */
        newpsr_exc = newpsr & XPSR_EXCP;
        if (!((env->v7m.exception == 0 && newpsr_exc == 0) ||
              (env->v7m.exception == 1 && newpsr_exc != 0))) {
            /* Pend the fault and tell our caller to take it */
            env->v7m.cfsr[env->v7m.secure] |= R_V7M_CFSR_INVPC_MASK;
            // Unicorn: commented out
            //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_USAGE,
            //                        env->v7m.secure);
            qemu_log_mask(CPU_LOG_INT,
                          "...taking INVPC UsageFault: "
                          "IPSR consistency check failed\n");
            return false;
        }

        *frame_sp_p = frameptr + 8;
    }

    /* This invalidates frame_sp_p */
    switch_v7m_security_state(env, true);
    env->v7m.exception = newpsr_exc;
    env->v7m.control[M_REG_S] &= ~R_V7M_CONTROL_SFPA_MASK;
    if (newpsr & XPSR_SFPA) {
        env->v7m.control[M_REG_S] |= R_V7M_CONTROL_SFPA_MASK;
    }
    xpsr_write(env, 0, XPSR_IT);
    env->thumb = newpc & 1;
    env->regs[15] = newpc & ~1;

    qemu_log_mask(CPU_LOG_INT, "...function return successful\n");
    return true;
}

static void arm_log_exception(int idx)
{
    if (qemu_loglevel_mask(CPU_LOG_INT)) {
        const char *exc = NULL;
        static const char * const excnames[] = {
            NULL,
            "Undefined Instruction",
            "SVC",
            "Prefetch Abort",
            "Data Abort",
            "IRQ",
            "FIQ",
            "Breakpoint",
            "QEMU v7M exception exit",
             "QEMU intercept of kernel commpage",
             NULL,
            "Hypervisor Call",
            "Hypervisor Trap",
            "Secure Monitor Call",
            "Virtual IRQ",
            "Virtual FIQ",
            "Semihosting call",
            "v7M NOCP UsageFault",
            "v7M INVSTATE UsageFault",
        };

        if (idx >= 0 && idx < ARRAY_SIZE(excnames)) {
            exc = excnames[idx];
        }
        if (!exc) {
            exc = "unknown";
        }
        qemu_log_mask(CPU_LOG_INT, "Taking exception %d [%s]\n", idx, exc);
    }
}

static bool v7m_read_half_insn(ARMCPU *cpu, ARMMMUIdx mmu_idx,
                               uint32_t addr, uint16_t *insn)
{
    /* Load a 16-bit portion of a v7M instruction, returning true on success,
     * or false on failure (in which case we will have pended the appropriate
     * exception).
     * We need to do the instruction fetch's MPU and SAU checks
     * like this because there is no MMU index that would allow
     * doing the load with a single function call. Instead we must
     * first check that the security attributes permit the load
     * and that they don't mismatch on the two halves of the instruction,
     * and then we do the load as a secure load (ie using the security
     * attributes of the address, not the CPU, as architecturally required).
     */
    CPUState *cs = CPU(cpu);
    CPUARMState *env = &cpu->env;
    V8M_SAttributes sattrs = {0};
    MemTxAttrs attrs = {0};
    ARMMMUFaultInfo fi = {0};
    MemTxResult txres;
    target_ulong page_size;
    hwaddr physaddr;
    int prot;

    v8m_security_lookup(env, addr, MMU_INST_FETCH, mmu_idx, &sattrs);
    if (!sattrs.nsc || sattrs.ns) {
        /* This must be the second half of the insn, and it straddles a
         * region boundary with the second half not being S&NSC.
         */
        env->v7m.sfsr |= R_V7M_SFSR_INVEP_MASK;
        // Unicorn: commented out
        //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_SECURE, false);
        qemu_log_mask(CPU_LOG_INT,
                      "...really SecureFault with SFSR.INVEP\n");
        return false;
    }
    if (get_phys_addr(env, addr, MMU_INST_FETCH, mmu_idx,
                      &physaddr, &attrs, &prot, &page_size, &fi, NULL)) {
        /* the MPU lookup failed */
        env->v7m.cfsr[env->v7m.secure] |= R_V7M_CFSR_IACCVIOL_MASK;
        // Unicorn: commented out
        //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_MEM, env->v7m.secure);
        qemu_log_mask(CPU_LOG_INT, "...really MemManage with CFSR.IACCVIOL\n");
        return false;
    }
    *insn = address_space_lduw_le(arm_addressspace(cs, attrs), physaddr,
                                 attrs, &txres);
    if (txres != MEMTX_OK) {
        env->v7m.cfsr[M_REG_NS] |= R_V7M_CFSR_IBUSERR_MASK;
        // Unicorn: commented out
        //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_BUS, false);
        qemu_log_mask(CPU_LOG_INT, "...really BusFault with CFSR.IBUSERR\n");
        return false;
    }
    return true;
}

static bool v7m_handle_execute_nsc(ARMCPU *cpu)
{
    /* Check whether this attempt to execute code in a Secure & NS-Callable
     * memory region is for an SG instruction; if so, then emulate the
     * effect of the SG instruction and return true. Otherwise pend
     * the correct kind of exception and return false.
     */
    CPUARMState *env = &cpu->env;
    ARMMMUIdx mmu_idx;
    uint16_t insn;

    /* We should never get here unless get_phys_addr_pmsav8() caused
     * an exception for NS executing in S&NSC memory.
     */
    assert(!env->v7m.secure);
    assert(arm_feature(env, ARM_FEATURE_M_SECURITY));

    /* We want to do the MPU lookup as secure; work out what mmu_idx that is */
    mmu_idx = arm_v7m_mmu_idx_for_secstate(env, true);

    if (!v7m_read_half_insn(cpu, mmu_idx, env->regs[15], &insn)) {
        return false;
    }

    if (!env->thumb) {
        goto gen_invep;
    }

    if (insn != 0xe97f) {
        /* Not an SG instruction first half (we choose the IMPDEF
         * early-SG-check option).
         */
        goto gen_invep;
    }

    if (!v7m_read_half_insn(cpu, mmu_idx, env->regs[15] + 2, &insn)) {
        return false;
    }

    if (insn != 0xe97f) {
        /* Not an SG instruction second half (yes, both halves of the SG
         * insn have the same hex value)
         */
        goto gen_invep;
    }

    /* OK, we have confirmed that we really have an SG instruction.
     * We know we're NS in S memory so don't need to repeat those checks.
     */
    qemu_log_mask(CPU_LOG_INT, "...really an SG instruction at 0x%08" PRIx32
                  ", executing it\n", env->regs[15]);
    env->regs[14] &= ~1;
    switch_v7m_security_state(env, true);
    xpsr_write(env, 0, XPSR_IT);
    env->regs[15] += 4;
    return true;

gen_invep:
    env->v7m.sfsr |= R_V7M_SFSR_INVEP_MASK;
    // Unicorn: commented out
    //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_SECURE, false);
    qemu_log_mask(CPU_LOG_INT,
                  "...really SecureFault with SFSR.INVEP\n");
    return false;
}

void arm_v7m_cpu_do_interrupt(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs->uc, cs);
    CPUARMState *env = cs->env_ptr;
    uint32_t lr;
    bool ignore_stackfaults;

    arm_log_exception(cs->exception_index);

    /* For exceptions we just mark as pending on the NVIC, and let that
       handle it.  */
    switch (cs->exception_index) {
    case EXCP_UDEF:
        //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_USAGE);
        env->v7m.cfsr[env->v7m.secure] |= R_V7M_CFSR_UNDEFINSTR_MASK;
        break;
    case EXCP_NOCP:
        //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_USAGE);
        env->v7m.cfsr[env->v7m.secure] |= R_V7M_CFSR_NOCP_MASK;
        break;
    case EXCP_INVSTATE:
        //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_USAGE);
        env->v7m.cfsr[env->v7m.secure] |= R_V7M_CFSR_INVSTATE_MASK;
        break;
    case EXCP_SWI:
        /* The PC already points to the next instruction.  */
        //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_SVC);
        break;
    case EXCP_PREFETCH_ABORT:
    case EXCP_DATA_ABORT:
        /* Note that for M profile we don't have a guest facing FSR, but
         * the env->exception.fsr will be populated by the code that
         * raises the fault, in the A profile short-descriptor format.
         */
        switch (env->exception.fsr & 0xf) {
        case M_FAKE_FSR_NSC_EXEC:
            /* Exception generated when we try to execute code at an address
             * which is marked as Secure & Non-Secure Callable and the CPU
             * is in the Non-Secure state. The only instruction which can
             * be executed like this is SG (and that only if both halves of
             * the SG instruction have the same security attributes.)
             * Everything else must generate an INVEP SecureFault, so we
             * emulate the SG instruction here.
             */
            if (v7m_handle_execute_nsc(cpu)) {
                return;
            }
            break;
        case M_FAKE_FSR_SFAULT:
            /* Various flavours of SecureFault for attempts to execute or
             * access data in the wrong security state.
             */
            switch (cs->exception_index) {
            case EXCP_PREFETCH_ABORT:
                if (env->v7m.secure) {
                    env->v7m.sfsr |= R_V7M_SFSR_INVTRAN_MASK;
                    qemu_log_mask(CPU_LOG_INT,
                                  "...really SecureFault with SFSR.INVTRAN\n");
                } else {
                    env->v7m.sfsr |= R_V7M_SFSR_INVEP_MASK;
                    qemu_log_mask(CPU_LOG_INT,
                                  "...really SecureFault with SFSR.INVEP\n");
                }
                break;
            case EXCP_DATA_ABORT:
                /* This must be an NS access to S memory */
                env->v7m.sfsr |= R_V7M_SFSR_AUVIOL_MASK;
                qemu_log_mask(CPU_LOG_INT,
                              "...really SecureFault with SFSR.AUVIOL\n");
                break;
            }
            // Unicorn: commented out
            //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_SECURE, false);
            break;
        case 0x8: /* External Abort */
            switch (cs->exception_index) {
            case EXCP_PREFETCH_ABORT:
                env->v7m.cfsr[M_REG_NS] |= R_V7M_CFSR_IBUSERR_MASK;
                qemu_log_mask(CPU_LOG_INT, "...with CFSR.IBUSERR\n");
                break;
            case EXCP_DATA_ABORT:
                env->v7m.cfsr[M_REG_NS] |=
                    (R_V7M_CFSR_PRECISERR_MASK | R_V7M_CFSR_BFARVALID_MASK);
                env->v7m.bfar = env->exception.vaddress;
                qemu_log_mask(CPU_LOG_INT,
                              "...with CFSR.PRECISERR and BFAR 0x%x\n",
                              env->v7m.bfar);
                break;
            }
            // Unicorn: commented out
            //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_BUS);
            break;
        default:
            /* All other FSR values are either MPU faults or "can't happen
             * for M profile" cases.
             */
            switch (cs->exception_index) {
            case EXCP_PREFETCH_ABORT:
                env->v7m.cfsr[env->v7m.secure] |= R_V7M_CFSR_IACCVIOL_MASK;
                qemu_log_mask(CPU_LOG_INT, "...with CFSR.IACCVIOL\n");
                break;
            case EXCP_DATA_ABORT:
                env->v7m.cfsr[env->v7m.secure] |=
                    (R_V7M_CFSR_DACCVIOL_MASK | R_V7M_CFSR_MMARVALID_MASK);
                env->v7m.mmfar[env->v7m.secure] = env->exception.vaddress;
                qemu_log_mask(CPU_LOG_INT,
                              "...with CFSR.DACCVIOL and MMFAR 0x%x\n",
                              env->v7m.mmfar[env->v7m.secure]);
                break;
            }
            // Unicorn: commented out
            //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_MEM);
            break;
        }
        break;
    case EXCP_BKPT:
#if 0
        if (semihosting_enabled) {
            int nr;
            nr = arm_lduw_code(env, env->regs[15], arm_sctlr_b(env)) & 0xff;
            if (nr == 0xab) {
                env->regs[15] += 2;
                qemu_log_mask(CPU_LOG_INT,
                              "...handling as semihosting call 0x%x\n",
                              env->regs[0]);
                env->regs[0] = do_arm_semihosting(env);
                return;
            }
        }
#endif
        //armv7m_nvic_set_pending(env->nvic, ARMV7M_EXCP_DEBUG);
        break;
    case EXCP_IRQ:
        break;
    case EXCP_EXCEPTION_EXIT:
        if (env->regs[15] < EXC_RETURN_MIN_MAGIC) {
            /* Must be v8M security extension function return */
            assert(env->regs[15] >= FNC_RETURN_MIN_MAGIC);
            assert(arm_feature(env, ARM_FEATURE_M_SECURITY));
            if (do_v7m_function_return(cpu)) {
                return;
            }
        } else {
            do_v7m_exception_exit(cpu);
            return;
        }
        break;
    default:
        cpu_abort(cs, "Unhandled exception 0x%x\n", cs->exception_index);
        return; /* Never happens.  Keep compiler happy.  */
    }

    if (arm_feature(env, ARM_FEATURE_V8)) {
        lr = R_V7M_EXCRET_RES1_MASK |
            R_V7M_EXCRET_DCRS_MASK |
            R_V7M_EXCRET_FTYPE_MASK;
        /* The S bit indicates whether we should return to Secure
         * or NonSecure (ie our current state).
         * The ES bit indicates whether we're taking this exception
         * to Secure or NonSecure (ie our target state). We set it
         * later, in v7m_exception_taken().
         * The SPSEL bit is also set in v7m_exception_taken() for v8M.
         * This corresponds to the ARM ARM pseudocode for v8M setting
         * some LR bits in PushStack() and some in ExceptionTaken();
         * the distinction matters for the tailchain cases where we
         * can take an exception without pushing the stack.
         */
        if (env->v7m.secure) {
            lr |= R_V7M_EXCRET_S_MASK;
        }
    } else {
        lr = R_V7M_EXCRET_RES1_MASK |
            R_V7M_EXCRET_S_MASK |
            R_V7M_EXCRET_DCRS_MASK |
            R_V7M_EXCRET_FTYPE_MASK |
            R_V7M_EXCRET_ES_MASK;
        if (env->v7m.control[M_REG_NS] & R_V7M_CONTROL_SPSEL_MASK) {
            lr |= R_V7M_EXCRET_SPSEL_MASK;
        }
    }
    if (!arm_v7m_is_handler_mode(env)) {
        lr |= R_V7M_EXCRET_MODE_MASK;
    }

    ignore_stackfaults = v7m_push_stack(cpu);
    v7m_exception_taken(cpu, lr, false, ignore_stackfaults);
    qemu_log_mask(CPU_LOG_INT, "... as %d\n", env->v7m.exception);
}

/* Function used to synchronize QEMU's AArch64 register set with AArch32
 * register set.  This is necessary when switching between AArch32 and AArch64
 * execution state.
 */
void aarch64_sync_32_to_64(CPUARMState *env)
{
    int i;
    uint32_t mode = env->uncached_cpsr & CPSR_M;

    /* We can blanket copy R[0:7] to X[0:7] */
    for (i = 0; i < 8; i++) {
        env->xregs[i] = env->regs[i];
    }

    /* Unless we are in FIQ mode, x8-x12 come from the user registers r8-r12.
     * Otherwise, they come from the banked user regs.
     */
    if (mode == ARM_CPU_MODE_FIQ) {
        for (i = 8; i < 13; i++) {
            env->xregs[i] = env->usr_regs[i - 8];
        }
    } else {
        for (i = 8; i < 13; i++) {
            env->xregs[i] = env->regs[i];
        }
    }

    /* Registers x13-x23 are the various mode SP and FP registers. Registers
     * r13 and r14 are only copied if we are in that mode, otherwise we copy
     * from the mode banked register.
     */
    if (mode == ARM_CPU_MODE_USR || mode == ARM_CPU_MODE_SYS) {
        env->xregs[13] = env->regs[13];
        env->xregs[14] = env->regs[14];
    } else {
        env->xregs[13] = env->banked_r13[bank_number(ARM_CPU_MODE_USR)];
        /* HYP is an exception in that it is copied from r14 */
        if (mode == ARM_CPU_MODE_HYP) {
            env->xregs[14] = env->regs[14];
        } else {
            env->xregs[14] = env->banked_r14[bank_number(ARM_CPU_MODE_USR)];
        }
    }

    if (mode == ARM_CPU_MODE_HYP) {
        env->xregs[15] = env->regs[13];
    } else {
        env->xregs[15] = env->banked_r13[bank_number(ARM_CPU_MODE_HYP)];
    }

    if (mode == ARM_CPU_MODE_IRQ) {
        env->xregs[16] = env->regs[14];
        env->xregs[17] = env->regs[13];
    } else {
        env->xregs[16] = env->banked_r14[bank_number(ARM_CPU_MODE_IRQ)];
        env->xregs[17] = env->banked_r13[bank_number(ARM_CPU_MODE_IRQ)];
    }

    if (mode == ARM_CPU_MODE_SVC) {
        env->xregs[18] = env->regs[14];
        env->xregs[19] = env->regs[13];
    } else {
        env->xregs[18] = env->banked_r14[bank_number(ARM_CPU_MODE_SVC)];
        env->xregs[19] = env->banked_r13[bank_number(ARM_CPU_MODE_SVC)];
    }

    if (mode == ARM_CPU_MODE_ABT) {
        env->xregs[20] = env->regs[14];
        env->xregs[21] = env->regs[13];
    } else {
        env->xregs[20] = env->banked_r14[bank_number(ARM_CPU_MODE_ABT)];
        env->xregs[21] = env->banked_r13[bank_number(ARM_CPU_MODE_ABT)];
    }

    if (mode == ARM_CPU_MODE_UND) {
        env->xregs[22] = env->regs[14];
        env->xregs[23] = env->regs[13];
    } else {
        env->xregs[22] = env->banked_r14[bank_number(ARM_CPU_MODE_UND)];
        env->xregs[23] = env->banked_r13[bank_number(ARM_CPU_MODE_UND)];
    }

    /* Registers x24-x30 are mapped to r8-r14 in FIQ mode.  If we are in FIQ
     * mode, then we can copy from r8-r14.  Otherwise, we copy from the
     * FIQ bank for r8-r14.
     */
    if (mode == ARM_CPU_MODE_FIQ) {
        for (i = 24; i < 31; i++) {
            env->xregs[i] = env->regs[i - 16];   /* X[24:30] <- R[8:14] */
        }
    } else {
        for (i = 24; i < 29; i++) {
            env->xregs[i] = env->fiq_regs[i - 24];
        }
        env->xregs[29] = env->banked_r13[bank_number(ARM_CPU_MODE_FIQ)];
        env->xregs[30] = env->banked_r14[bank_number(ARM_CPU_MODE_FIQ)];
    }

    env->pc = env->regs[15];
}

/* Function used to synchronize QEMU's AArch32 register set with AArch64
 * register set.  This is necessary when switching between AArch32 and AArch64
 * execution state.
 */
void aarch64_sync_64_to_32(CPUARMState *env)
{
    int i;
    uint32_t mode = env->uncached_cpsr & CPSR_M;

    /* We can blanket copy X[0:7] to R[0:7] */
    for (i = 0; i < 8; i++) {
        env->regs[i] = env->xregs[i];
    }

    /* Unless we are in FIQ mode, r8-r12 come from the user registers x8-x12.
     * Otherwise, we copy x8-x12 into the banked user regs.
     */
    if (mode == ARM_CPU_MODE_FIQ) {
        for (i = 8; i < 13; i++) {
            env->usr_regs[i - 8] = env->xregs[i];
        }
    } else {
        for (i = 8; i < 13; i++) {
            env->regs[i] = env->xregs[i];
        }
    }

    /* Registers r13 & r14 depend on the current mode.
     * If we are in a given mode, we copy the corresponding x registers to r13
     * and r14.  Otherwise, we copy the x register to the banked r13 and r14
     * for the mode.
     */
    if (mode == ARM_CPU_MODE_USR || mode == ARM_CPU_MODE_SYS) {
        env->regs[13] = env->xregs[13];
        env->regs[14] = env->xregs[14];
    } else {
        env->banked_r13[bank_number(ARM_CPU_MODE_USR)] = env->xregs[13];

        /* HYP is an exception in that it does not have its own banked r14 but
         * shares the USR r14
         */
        if (mode == ARM_CPU_MODE_HYP) {
            env->regs[14] = env->xregs[14];
        } else {
            env->banked_r14[bank_number(ARM_CPU_MODE_USR)] = env->xregs[14];
        }
    }

    if (mode == ARM_CPU_MODE_HYP) {
        env->regs[13] = env->xregs[15];
    } else {
        env->banked_r13[bank_number(ARM_CPU_MODE_HYP)] = env->xregs[15];
    }

    if (mode == ARM_CPU_MODE_IRQ) {
        env->regs[14] = env->xregs[16];
        env->regs[13] = env->xregs[17];
    } else {
        env->banked_r14[bank_number(ARM_CPU_MODE_IRQ)] = env->xregs[16];
        env->banked_r13[bank_number(ARM_CPU_MODE_IRQ)] = env->xregs[17];
    }

    if (mode == ARM_CPU_MODE_SVC) {
        env->regs[14] = env->xregs[18];
        env->regs[13] = env->xregs[19];
    } else {
        env->banked_r14[bank_number(ARM_CPU_MODE_SVC)] = env->xregs[18];
        env->banked_r13[bank_number(ARM_CPU_MODE_SVC)] = env->xregs[19];
    }

    if (mode == ARM_CPU_MODE_ABT) {
        env->regs[14] = env->xregs[20];
        env->regs[13] = env->xregs[21];
    } else {
        env->banked_r14[bank_number(ARM_CPU_MODE_ABT)] = env->xregs[20];
        env->banked_r13[bank_number(ARM_CPU_MODE_ABT)] = env->xregs[21];
    }

    if (mode == ARM_CPU_MODE_UND) {
        env->regs[14] = env->xregs[22];
        env->regs[13] = env->xregs[23];
    } else {
        env->banked_r14[bank_number(ARM_CPU_MODE_UND)] = env->xregs[22];
        env->banked_r13[bank_number(ARM_CPU_MODE_UND)] = env->xregs[23];
    }

    /* Registers x24-x30 are mapped to r8-r14 in FIQ mode.  If we are in FIQ
     * mode, then we can copy to r8-r14.  Otherwise, we copy to the
     * FIQ bank for r8-r14.
     */
    if (mode == ARM_CPU_MODE_FIQ) {
        for (i = 24; i < 31; i++) {
            env->regs[i - 16] = env->xregs[i];   /* X[24:30] -> R[8:14] */
        }
    } else {
        for (i = 24; i < 29; i++) {
            env->fiq_regs[i - 24] = env->xregs[i];
        }
        env->banked_r13[bank_number(ARM_CPU_MODE_FIQ)] = env->xregs[29];
        env->banked_r14[bank_number(ARM_CPU_MODE_FIQ)] = env->xregs[30];
    }

    env->regs[15] = env->pc;
}

// Unicorn: underscore appended to prevent silly clashing with defines
static void arm_cpu_do_interrupt_aarch32_(CPUState *cs)
{
    CPUARMState *env = cs->env_ptr;
    uint32_t addr;
    uint32_t mask;
    int new_mode;
    uint32_t offset;
    uint32_t moe;

    /* If this is a debug exception we must update the DBGDSCR.MOE bits */
    switch (env->exception.syndrome >> ARM_EL_EC_SHIFT) {
    case EC_BREAKPOINT:
    case EC_BREAKPOINT_SAME_EL:
        moe = 1;
        break;
    case EC_WATCHPOINT:
    case EC_WATCHPOINT_SAME_EL:
        moe = 10;
        break;
    case EC_AA32_BKPT:
        moe = 3;
        break;
    case EC_VECTORCATCH:
        moe = 5;
        break;
    default:
        moe = 0;
        break;
    }

    if (moe) {
        env->cp15.mdscr_el1 = deposit64(env->cp15.mdscr_el1, 2, 4, moe);
    }

    /* TODO: Vectored interrupt controller.  */
    switch (cs->exception_index) {
    case EXCP_UDEF:
        new_mode = ARM_CPU_MODE_UND;
        addr = 0x04;
        mask = CPSR_I;
        if (env->thumb)
            offset = 2;
        else
            offset = 4;
        break;
    case EXCP_SWI:
        new_mode = ARM_CPU_MODE_SVC;
        addr = 0x08;
        mask = CPSR_I;
        /* The PC already points to the next instruction.  */
        offset = 0;
        break;
    case EXCP_BKPT:
        env->exception.fsr = 2;
        /* Fall through to prefetch abort.  */
    case EXCP_PREFETCH_ABORT:
        A32_BANKED_CURRENT_REG_SET(env, ifsr, env->exception.fsr);
        A32_BANKED_CURRENT_REG_SET(env, ifar, env->exception.vaddress);
        qemu_log_mask(CPU_LOG_INT, "...with IFSR 0x%x IFAR 0x%x\n",
                      env->exception.fsr, (uint32_t)env->exception.vaddress);
        new_mode = ARM_CPU_MODE_ABT;
        addr = 0x0c;
        mask = CPSR_A | CPSR_I;
        offset = 4;
        break;
    case EXCP_DATA_ABORT:
        A32_BANKED_CURRENT_REG_SET(env, dfsr, env->exception.fsr);
        A32_BANKED_CURRENT_REG_SET(env, dfar, env->exception.vaddress);
        qemu_log_mask(CPU_LOG_INT, "...with DFSR 0x%x DFAR 0x%x\n",
                      env->exception.fsr,
                      (uint32_t)env->exception.vaddress);
        new_mode = ARM_CPU_MODE_ABT;
        addr = 0x10;
        mask = CPSR_A | CPSR_I;
        offset = 8;
        break;
    case EXCP_IRQ:
        new_mode = ARM_CPU_MODE_IRQ;
        addr = 0x18;
        /* Disable IRQ and imprecise data aborts.  */
        mask = CPSR_A | CPSR_I;
        offset = 4;
        if (env->cp15.scr_el3 & SCR_IRQ) {
            /* IRQ routed to monitor mode */
            new_mode = ARM_CPU_MODE_MON;
            mask |= CPSR_F;
        }
        break;
    case EXCP_FIQ:
        new_mode = ARM_CPU_MODE_FIQ;
        addr = 0x1c;
        /* Disable FIQ, IRQ and imprecise data aborts.  */
        mask = CPSR_A | CPSR_I | CPSR_F;
        if (env->cp15.scr_el3 & SCR_FIQ) {
            /* FIQ routed to monitor mode */
            new_mode = ARM_CPU_MODE_MON;
        }
        offset = 4;
        break;
    case EXCP_VIRQ:
        new_mode = ARM_CPU_MODE_IRQ;
        addr = 0x18;
        /* Disable IRQ and imprecise data aborts.  */
        mask = CPSR_A | CPSR_I;
        offset = 4;
        break;
    case EXCP_VFIQ:
        new_mode = ARM_CPU_MODE_FIQ;
        addr = 0x1c;
        /* Disable FIQ, IRQ and imprecise data aborts.  */
        mask = CPSR_A | CPSR_I | CPSR_F;
        offset = 4;
        break;
    case EXCP_SMC:
        new_mode = ARM_CPU_MODE_MON;
        addr = 0x08;
        mask = CPSR_A | CPSR_I | CPSR_F;
        offset = 0;
        break;
    default:
        cpu_abort(cs, "Unhandled exception 0x%x\n", cs->exception_index);
        return; /* Never happens.  Keep compiler happy.  */
    }

    if (new_mode == ARM_CPU_MODE_MON) {
        addr += env->cp15.mvbar;
    } else if (A32_BANKED_CURRENT_REG_GET(env, sctlr) & SCTLR_V) {
        /* High vectors. When enabled, base address cannot be remapped. */
        addr += 0xffff0000;
    } else {
        /* ARM v7 architectures provide a vector base address register to remap
         * the interrupt vector table.
         * This register is only followed in non-monitor mode, and is banked.
         * Note: only bits 31:5 are valid.
         */
        addr += A32_BANKED_CURRENT_REG_GET(env, vbar);
    }

    if ((env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_MON) {
        env->cp15.scr_el3 &= ~SCR_NS;
    }

    switch_mode (env, new_mode);
    /* For exceptions taken to AArch32 we must clear the SS bit in both
     * PSTATE and in the old-state value we save to SPSR_<mode>, so zero it now.
     */
    env->uncached_cpsr &= ~PSTATE_SS;
    env->spsr = cpsr_read(env);
    /* Clear IT bits.  */
    env->condexec_bits = 0;
    /* Switch to the new mode, and to the correct instruction set.  */
    env->uncached_cpsr = (env->uncached_cpsr & ~CPSR_M) | new_mode;
    /* Set new mode endianness */
    env->uncached_cpsr &= ~CPSR_E;
    if (env->cp15.sctlr_el[arm_current_el(env)] & SCTLR_EE) {
        env->uncached_cpsr |= CPSR_E;
    }
    env->daif |= mask;
    /* this is a lie, as the was no c1_sys on V4T/V5, but who cares
     * and we should just guard the thumb mode on V4 */
    if (arm_feature(env, ARM_FEATURE_V4T)) {
        env->thumb = (A32_BANKED_CURRENT_REG_GET(env, sctlr) & SCTLR_TE) != 0;
    }
    env->regs[14] = env->regs[15] + offset;
    env->regs[15] = addr;
}

/* Handle exception entry to a target EL which is using AArch64 */
// Unicorn: underscore appended to prevent silly clashing with defines
static void arm_cpu_do_interrupt_aarch64_(CPUState *cs)
{
    CPUARMState *env = cs->env_ptr;
    unsigned int new_el = env->exception.target_el;
    target_ulong addr = env->cp15.vbar_el[new_el];
    unsigned int new_mode = aarch64_pstate_mode(new_el, true);

    if (arm_current_el(env) < new_el) {
        /* Entry vector offset depends on whether the implemented EL
         * immediately lower than the target level is using AArch32 or AArch64
         */
        bool is_aa64 = false;

        switch (new_el) {
        case 3:
            is_aa64 = (env->cp15.scr_el3 & SCR_RW) != 0;
            break;
        case 2:
            is_aa64 = (env->cp15.hcr_el2 & HCR_RW) != 0;
            break;
        case 1:
            is_aa64 = is_a64(env);
            break;
        default:
            g_assert_not_reached();
        }

        if (is_aa64) {
            addr += 0x400;
        } else {
            addr += 0x600;
        }
    } else if (pstate_read(env) & PSTATE_SP) {
        addr += 0x200;
    }

    switch (cs->exception_index) {
    case EXCP_PREFETCH_ABORT:
    case EXCP_DATA_ABORT:
        env->cp15.far_el[new_el] = env->exception.vaddress;
        qemu_log_mask(CPU_LOG_INT, "...with FAR 0x%" PRIx64 "\n",
                      env->cp15.far_el[new_el]);
        /* fall through */
    case EXCP_BKPT:
    case EXCP_UDEF:
    case EXCP_SWI:
    case EXCP_HVC:
    case EXCP_HYP_TRAP:
    case EXCP_SMC:
        env->cp15.esr_el[new_el] = env->exception.syndrome;
        break;
    case EXCP_IRQ:
    case EXCP_VIRQ:
        addr += 0x80;
        break;
    case EXCP_FIQ:
    case EXCP_VFIQ:
        addr += 0x100;
        break;
    case EXCP_SEMIHOST:
        /* UNICORN: Commented out
        qemu_log_mask(CPU_LOG_INT,
                      "...handling as semihosting call 0x%" PRIx64 "\n",
                      env->xregs[0]);
        env->xregs[0] = do_arm_semihosting(env);*/
        return;
    default:
        cpu_abort(cs, "Unhandled exception 0x%x\n", cs->exception_index);
    }

    if (is_a64(env)) {
        env->banked_spsr[aarch64_banked_spsr_index(new_el)] = pstate_read(env);
        aarch64_save_sp(env, arm_current_el(env));
        env->elr_el[new_el] = env->pc;
    } else {
        env->banked_spsr[aarch64_banked_spsr_index(new_el)] = cpsr_read(env);
        env->elr_el[new_el] = env->regs[15];

        aarch64_sync_32_to_64(env);

        env->condexec_bits = 0;
    }
    qemu_log_mask(CPU_LOG_INT, "...with ELR 0x%" PRIx64 "\n",
                  env->elr_el[new_el]);

    pstate_write(env, PSTATE_DAIF | new_mode);
    env->aarch64 = 1;
    aarch64_restore_sp(env, new_el);

    env->pc = addr;

    qemu_log_mask(CPU_LOG_INT, "...to EL%d PC 0x%" PRIx64 " PSTATE 0x%x\n",
                  new_el, env->pc, pstate_read(env));
}

static inline bool check_for_semihosting(CPUState *cs)
{
    return false;

// Unicorn: ifdefd out
#if 0
    /* Check whether this exception is a semihosting call; if so
     * then handle it and return true; otherwise return false.
     */
    ARMCPU *cpu = ARM_CPU(cs);
    CPUARMState *env = &cpu->env;

    if (is_a64(env)) {
        if (cs->exception_index == EXCP_SEMIHOST) {
            /* This is always the 64-bit semihosting exception.
             * The "is this usermode" and "is semihosting enabled"
             * checks have been done at translate time.
             */
            qemu_log_mask(CPU_LOG_INT,
                          "...handling as semihosting call 0x%" PRIx64 "\n",
                          env->xregs[0]);
            env->xregs[0] = do_arm_semihosting(env);
            return true;
        }
        return false;
    } else {
        uint32_t imm;

        /* Only intercept calls from privileged modes, to provide some
         * semblance of security.
         */
        if (cs->exception_index != EXCP_SEMIHOST &&
            (!semihosting_enabled() ||
             ((env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_USR))) {
            return false;
        }

        switch (cs->exception_index) {
        case EXCP_SEMIHOST:
            /* This is always a semihosting call; the "is this usermode"
             * and "is semihosting enabled" checks have been done at
             * translate time.
             */
            break;
        case EXCP_SWI:
            /* Check for semihosting interrupt.  */
            if (env->thumb) {
                imm = arm_lduw_code(env, env->regs[15] - 2, arm_sctlr_b(env))
                    & 0xff;
                if (imm == 0xab) {
                    break;
                }
            } else {
                imm = arm_ldl_code(env, env->regs[15] - 4, arm_sctlr_b(env))
                    & 0xffffff;
                if (imm == 0x123456) {
                    break;
                }
            }
            return false;
        case EXCP_BKPT:
            /* See if this is a semihosting syscall.  */
            if (env->thumb) {
                imm = arm_lduw_code(env, env->regs[15], arm_sctlr_b(env))
                    & 0xff;
                if (imm == 0xab) {
                    env->regs[15] += 2;
                    break;
                }
            }
            return false;
        default:
            return false;
        }

        qemu_log_mask(CPU_LOG_INT,
                      "...handling as semihosting call 0x%x\n",
                      env->regs[0]);
        env->regs[0] = do_arm_semihosting(env);
        return true;
    }
#endif
}

/* Handle a CPU exception for A and R profile CPUs.
 * Do any appropriate logging, handle PSCI calls, and then hand off
 * to the AArch64-entry or AArch32-entry function depending on the
 * target exception level's register width.
 */
void arm_cpu_do_interrupt(CPUState *cs)
{
    ARMCPU *cpu = ARM_CPU(cs->uc, cs);
    CPUARMState *env = &cpu->env;
    unsigned int new_el = env->exception.target_el;

    assert(!arm_feature(env, ARM_FEATURE_M));

    arm_log_exception(cs->exception_index);
    qemu_log_mask(CPU_LOG_INT, "...from EL%d to EL%d\n", arm_current_el(env),
                  new_el);
    if (qemu_loglevel_mask(CPU_LOG_INT)
        && !excp_is_internal(cs->exception_index)) {
        qemu_log_mask(CPU_LOG_INT, "...with ESR 0x%x/0x%" PRIx32 "\n",
                      env->exception.syndrome >> ARM_EL_EC_SHIFT,
                      env->exception.syndrome);
    }

    if (arm_is_psci_call(cpu, cs->exception_index)) {
        arm_handle_psci_call(cpu);
        qemu_log_mask(CPU_LOG_INT, "...handled as PSCI call\n");
        return;
    }

    /* Semihosting semantics depend on the register width of the
     * code that caused the exception, not the target exception level,
     * so must be handled here.
     */
    if (check_for_semihosting(cs)) {
        return;
    }

    assert(!excp_is_internal(cs->exception_index));
    if (arm_el_is_aa64(env, new_el)) {
        arm_cpu_do_interrupt_aarch64_(cs);
    } else {
        arm_cpu_do_interrupt_aarch32_(cs);
    }

    arm_call_el_change_hook(cpu);

    // Unicorn: commented out
    //if (!kvm_enabled()) {
    cs->interrupt_request |= CPU_INTERRUPT_EXITTB;
    //}
}

/* Return the exception level which controls this address translation regime */
static inline uint32_t regime_el(CPUARMState *env, ARMMMUIdx mmu_idx)
{
    switch (mmu_idx) {
    case ARMMMUIdx_S2NS:
    case ARMMMUIdx_S1E2:
        return 2;
    case ARMMMUIdx_S1E3:
        return 3;
    case ARMMMUIdx_S1SE0:
        return arm_el_is_aa64(env, 3) ? 1 : 3;
    case ARMMMUIdx_S1SE1:
    case ARMMMUIdx_S1NSE0:
    case ARMMMUIdx_S1NSE1:
    case ARMMMUIdx_MPrivNegPri:
    case ARMMMUIdx_MUserNegPri:
    case ARMMMUIdx_MPriv:
    case ARMMMUIdx_MUser:
    case ARMMMUIdx_MSPrivNegPri:
    case ARMMMUIdx_MSUserNegPri:
    case ARMMMUIdx_MSPriv:
    case ARMMMUIdx_MSUser:
        return 1;
    default:
        g_assert_not_reached();
    }
}

/* Return the SCTLR value which controls this address translation regime */
static inline uint32_t regime_sctlr(CPUARMState *env, ARMMMUIdx mmu_idx)
{
    return env->cp15.sctlr_el[regime_el(env, mmu_idx)];
}

/* Return true if the specified stage of address translation is disabled */
static inline bool regime_translation_disabled(CPUARMState *env,
                                               ARMMMUIdx mmu_idx)
{
    if (arm_feature(env, ARM_FEATURE_M)) {
        switch (env->v7m.mpu_ctrl[regime_is_secure(env, mmu_idx)] &
                (R_V7M_MPU_CTRL_ENABLE_MASK | R_V7M_MPU_CTRL_HFNMIENA_MASK)) {
        case R_V7M_MPU_CTRL_ENABLE_MASK:
            /* Enabled, but not for HardFault and NMI */
            return mmu_idx & ARM_MMU_IDX_M_NEGPRI;
        case R_V7M_MPU_CTRL_ENABLE_MASK | R_V7M_MPU_CTRL_HFNMIENA_MASK:
            /* Enabled for all cases */
            return false;
        case 0:
        default:
            /* HFNMIENA set and ENABLE clear is UNPREDICTABLE, but
             * we warned about that in armv7m_nvic.c when the guest set it.
             */
            return true;
        }
    }

    if (mmu_idx == ARMMMUIdx_S2NS) {
        return (env->cp15.hcr_el2 & HCR_VM) == 0;
    }
    return (regime_sctlr(env, mmu_idx) & SCTLR_M) == 0;
}

static inline bool regime_translation_big_endian(CPUARMState *env,
                                                 ARMMMUIdx mmu_idx)
{
    return (regime_sctlr(env, mmu_idx) & SCTLR_EE) != 0;
}

/* Return the TCR controlling this translation regime */
static inline TCR *regime_tcr(CPUARMState *env, ARMMMUIdx mmu_idx)
{
    if (mmu_idx == ARMMMUIdx_S2NS) {
        return &env->cp15.vtcr_el2;
    }
    return &env->cp15.tcr_el[regime_el(env, mmu_idx)];
}

/* Convert a possible stage1+2 MMU index into the appropriate
 * stage 1 MMU index
 */
static inline ARMMMUIdx stage_1_mmu_idx(ARMMMUIdx mmu_idx)
{
    if (mmu_idx == ARMMMUIdx_S12NSE0 || mmu_idx == ARMMMUIdx_S12NSE1) {
        mmu_idx += (ARMMMUIdx_S1NSE0 - ARMMMUIdx_S12NSE0);
    }
    return mmu_idx;
}

/* Returns TBI0 value for current regime el */
uint32_t arm_regime_tbi0(CPUARMState *env, ARMMMUIdx mmu_idx)
{
    TCR *tcr;
    uint32_t el;

    /* For EL0 and EL1, TBI is controlled by stage 1's TCR, so convert
     * a stage 1+2 mmu index into the appropriate stage 1 mmu index.
     */
    mmu_idx = stage_1_mmu_idx(mmu_idx);

    tcr = regime_tcr(env, mmu_idx);
    el = regime_el(env, mmu_idx);

    if (el > 1) {
        return extract64(tcr->raw_tcr, 20, 1);
    } else {
        return extract64(tcr->raw_tcr, 37, 1);
    }
}

/* Returns TBI1 value for current regime el */
uint32_t arm_regime_tbi1(CPUARMState *env, ARMMMUIdx mmu_idx)
{
    TCR *tcr;
    uint32_t el;

    /* For EL0 and EL1, TBI is controlled by stage 1's TCR, so convert
     * a stage 1+2 mmu index into the appropriate stage 1 mmu index.
     */
    mmu_idx = stage_1_mmu_idx(mmu_idx);

    tcr = regime_tcr(env, mmu_idx);
    el = regime_el(env, mmu_idx);

    if (el > 1) {
        return 0;
    } else {
        return extract64(tcr->raw_tcr, 38, 1);
    }
}

/* Return the TTBR associated with this translation regime */
static inline uint64_t regime_ttbr(CPUARMState *env, ARMMMUIdx mmu_idx,
                                   int ttbrn)
{
    if (mmu_idx == ARMMMUIdx_S2NS) {
        return env->cp15.vttbr_el2;
    }
    if (ttbrn == 0) {
        return env->cp15.ttbr0_el[regime_el(env, mmu_idx)];
    } else {
        return env->cp15.ttbr1_el[regime_el(env, mmu_idx)];
    }
}

/* Return true if the translation regime is using LPAE format page tables */
static inline bool regime_using_lpae_format(CPUARMState *env,
                                            ARMMMUIdx mmu_idx)
{
    int el = regime_el(env, mmu_idx);
    if (el == 2 || arm_el_is_aa64(env, el)) {
        return true;
    }
    if (arm_feature(env, ARM_FEATURE_LPAE)
        && (regime_tcr(env, mmu_idx)->raw_tcr & TTBCR_EAE)) {
        return true;
    }
    return false;
}

/* Returns true if the stage 1 translation regime is using LPAE format page
 * tables. Used when raising alignment exceptions, whose FSR changes depending
 * on whether the long or short descriptor format is in use. */
bool arm_s1_regime_using_lpae_format(CPUARMState *env, ARMMMUIdx mmu_idx)
{
    mmu_idx = stage_1_mmu_idx(mmu_idx);

    return regime_using_lpae_format(env, mmu_idx);
}

static inline bool regime_is_user(CPUARMState *env, ARMMMUIdx mmu_idx)
{
    switch (mmu_idx) {
    case ARMMMUIdx_S1SE0:
    case ARMMMUIdx_S1NSE0:
    case ARMMMUIdx_MUser:
    case ARMMMUIdx_MSUser:
    case ARMMMUIdx_MUserNegPri:
    case ARMMMUIdx_MSUserNegPri:
        return true;
    default:
        return false;
    case ARMMMUIdx_S12NSE0:
    case ARMMMUIdx_S12NSE1:
        g_assert_not_reached();
    }
}

/* Translate section/page access permissions to page
 * R/W protection flags
 *
 * @env:         CPUARMState
 * @mmu_idx:     MMU index indicating required translation regime
 * @ap:          The 3-bit access permissions (AP[2:0])
 * @domain_prot: The 2-bit domain access permissions
 */
static inline int ap_to_rw_prot(CPUARMState *env, ARMMMUIdx mmu_idx,
                                int ap, int domain_prot)
{
    bool is_user = regime_is_user(env, mmu_idx);

    if (domain_prot == 3) {
        return PAGE_READ | PAGE_WRITE;
    }

    switch (ap) {
    case 0:
        if (arm_feature(env, ARM_FEATURE_V7)) {
            return 0;
        }
        switch (regime_sctlr(env, mmu_idx) & (SCTLR_S | SCTLR_R)) {
        case SCTLR_S:
            return is_user ? 0 : PAGE_READ;
        case SCTLR_R:
            return PAGE_READ;
        default:
            return 0;
        }
    case 1:
        return is_user ? 0 : PAGE_READ | PAGE_WRITE;
    case 2:
        if (is_user) {
            return PAGE_READ;
        } else {
            return PAGE_READ | PAGE_WRITE;
        }
    case 3:
        return PAGE_READ | PAGE_WRITE;
    case 4: /* Reserved.  */
        return 0;
    case 5:
        return is_user ? 0 : PAGE_READ;
    case 6:
        return PAGE_READ;
    case 7:
        if (!arm_feature(env, ARM_FEATURE_V6K)) {
            return 0;
        }
        return PAGE_READ;
    default:
        g_assert_not_reached();
    }
}

/* Translate section/page access permissions to page
 * R/W protection flags.
 *
 * @ap:      The 2-bit simple AP (AP[2:1])
 * @is_user: TRUE if accessing from PL0
 */
static inline int simple_ap_to_rw_prot_is_user(int ap, bool is_user)
{
    switch (ap) {
    case 0:
        return is_user ? 0 : PAGE_READ | PAGE_WRITE;
    case 1:
        return PAGE_READ | PAGE_WRITE;
    case 2:
        return is_user ? 0 : PAGE_READ;
    case 3:
        return PAGE_READ;
    default:
        g_assert_not_reached();
        return 0;
    }
}

static inline int
simple_ap_to_rw_prot(CPUARMState *env, ARMMMUIdx mmu_idx, int ap)
{
    return simple_ap_to_rw_prot_is_user(ap, regime_is_user(env, mmu_idx));
}

/* Translate S2 section/page access permissions to protection flags
 *
 * @env:     CPUARMState
 * @s2ap:    The 2-bit stage2 access permissions (S2AP)
 * @xn:      XN (execute-never) bit
 */
static int get_S2prot(CPUARMState *env, int s2ap, int xn)
{
    int prot = 0;

    if (s2ap & 1) {
        prot |= PAGE_READ;
    }
    if (s2ap & 2) {
        prot |= PAGE_WRITE;
    }
    if (!xn) {
        if (arm_el_is_aa64(env, 2) || prot & PAGE_READ) {
            prot |= PAGE_EXEC;
        }
    }
    return prot;
}

/* Translate section/page access permissions to protection flags
 *
 * @env:     CPUARMState
 * @mmu_idx: MMU index indicating required translation regime
 * @is_aa64: TRUE if AArch64
 * @ap:      The 2-bit simple AP (AP[2:1])
 * @ns:      NS (non-secure) bit
 * @xn:      XN (execute-never) bit
 * @pxn:     PXN (privileged execute-never) bit
 */
static int get_S1prot(CPUARMState *env, ARMMMUIdx mmu_idx, bool is_aa64,
                      int ap, int ns, int xn, int pxn)
{
    bool is_user = regime_is_user(env, mmu_idx);
    int prot_rw, user_rw;
    bool have_wxn;
    int wxn = 0;

    assert(mmu_idx != ARMMMUIdx_S2NS);

    user_rw = simple_ap_to_rw_prot_is_user(ap, true);
    if (is_user) {
        prot_rw = user_rw;
    } else {
        prot_rw = simple_ap_to_rw_prot_is_user(ap, false);
    }

    if (ns && arm_is_secure(env) && (env->cp15.scr_el3 & SCR_SIF)) {
        return prot_rw;
    }

    /* TODO have_wxn should be replaced with
     *   ARM_FEATURE_V8 || (ARM_FEATURE_V7 && ARM_FEATURE_EL2)
     * when ARM_FEATURE_EL2 starts getting set. For now we assume all LPAE
     * compatible processors have EL2, which is required for [U]WXN.
     */
    have_wxn = arm_feature(env, ARM_FEATURE_LPAE);

    if (have_wxn) {
        wxn = regime_sctlr(env, mmu_idx) & SCTLR_WXN;
    }

    if (is_aa64) {
        switch (regime_el(env, mmu_idx)) {
        case 1:
            if (!is_user) {
                xn = pxn || (user_rw & PAGE_WRITE);
            }
            break;
        case 2:
        case 3:
            break;
        }
    } else if (arm_feature(env, ARM_FEATURE_V7)) {
        switch (regime_el(env, mmu_idx)) {
        case 1:
        case 3:
            if (is_user) {
                xn = xn || !(user_rw & PAGE_READ);
            } else {
                int uwxn = 0;
                if (have_wxn) {
                    uwxn = regime_sctlr(env, mmu_idx) & SCTLR_UWXN;
                }
                xn = xn || !(prot_rw & PAGE_READ) || pxn ||
                     (uwxn && (user_rw & PAGE_WRITE));
            }
            break;
        case 2:
            break;
        }
    } else {
        xn = wxn = 0;
    }

    if (xn || (wxn && (prot_rw & PAGE_WRITE))) {
        return prot_rw;
    }
    return prot_rw | PAGE_EXEC;
}

static bool get_level1_table_address(CPUARMState *env, ARMMMUIdx mmu_idx,
                                     uint32_t *table, uint32_t address)
{
    /* Note that we can only get here for an AArch32 PL0/PL1 lookup */
    TCR *tcr = regime_tcr(env, mmu_idx);

    if (address & tcr->mask) {
        if (tcr->raw_tcr & TTBCR_PD1) {
            /* Translation table walk disabled for TTBR1 */
            return false;
        }
        *table = regime_ttbr(env, mmu_idx, 1) & 0xffffc000;
    } else {
        if (tcr->raw_tcr & TTBCR_PD0) {
            /* Translation table walk disabled for TTBR0 */
            return false;
        }
        *table = regime_ttbr(env, mmu_idx, 0) & tcr->base_mask;
    }
    *table |= (address >> 18) & 0x3ffc;
    return true;
}

/* Translate a S1 pagetable walk through S2 if needed.  */
static hwaddr S1_ptw_translate(CPUARMState *env, ARMMMUIdx mmu_idx,
                               hwaddr addr, MemTxAttrs txattrs,
                               ARMMMUFaultInfo *fi)
{
    if ((mmu_idx == ARMMMUIdx_S1NSE0 || mmu_idx == ARMMMUIdx_S1NSE1) &&
        !regime_translation_disabled(env, ARMMMUIdx_S2NS)) {
        target_ulong s2size;
        hwaddr s2pa;
        int s2prot;
        int ret;

        ret = get_phys_addr_lpae(env, addr, 0, ARMMMUIdx_S2NS, &s2pa,
                                 &txattrs, &s2prot, &s2size, fi, NULL);
        if (ret) {
            assert(fi->type != ARMFault_None);
            fi->s2addr = addr;
            fi->stage2 = true;
            fi->s1ptw = true;
            return ~0;
        }
        addr = s2pa;
    }
    return addr;
}

/* All loads done in the course of a page table walk go through here.
 * TODO: rather than ignoring errors from physical memory reads (which
 * are external aborts in ARM terminology) we should propagate this
 * error out so that we can turn it into a Data Abort if this walk
 * was being done for a CPU load/store or an address translation instruction
 * (but not if it was for a debug access).
 */
static uint32_t arm_ldl_ptw(CPUState *cs, hwaddr addr, bool is_secure,
                            ARMMMUIdx mmu_idx, ARMMMUFaultInfo *fi)
{
    ARMCPU *cpu = ARM_CPU(cs->uc, cs);
    CPUARMState *env = &cpu->env;
    MemTxAttrs attrs = {0};
    MemTxResult result = MEMTX_OK;
    AddressSpace *as;
    uint32_t data;

    attrs.secure = is_secure;
    as = arm_addressspace(cs, attrs);
    addr = S1_ptw_translate(env, mmu_idx, addr, attrs, fi);
    if (fi->s1ptw) {
        return 0;
    }
    if (regime_translation_big_endian(env, mmu_idx)) {
        data = address_space_ldl_be(as, addr, attrs, &result);
    } else {
        data = address_space_ldl_le(as, addr, attrs, &result);
    }
    if (result == MEMTX_OK) {
        return data;
    }
    fi->type = ARMFault_SyncExternalOnWalk;
    fi->ea = arm_extabort_type(result);
    return 0;
}

static uint64_t arm_ldq_ptw(CPUState *cs, hwaddr addr, bool is_secure,
                            ARMMMUIdx mmu_idx, ARMMMUFaultInfo *fi)
{
    ARMCPU *cpu = ARM_CPU(cs->uc, cs);
    CPUARMState *env = &cpu->env;
    MemTxAttrs attrs = {0};
    MemTxResult result = MEMTX_OK;
    AddressSpace *as;
    uint64_t data;

    attrs.secure = is_secure;
    as = arm_addressspace(cs, attrs);
    addr = S1_ptw_translate(env, mmu_idx, addr, attrs, fi);
    if (fi->s1ptw) {
        return 0;
    }
    if (regime_translation_big_endian(env, mmu_idx)) {
        data = address_space_ldq_be(as, addr, attrs, &result);
    } else {
        data = address_space_ldq_le(as, addr, attrs, &result);
    }
    if (result == MEMTX_OK) {
        return data;
    }
    fi->type = ARMFault_SyncExternalOnWalk;
    fi->ea = arm_extabort_type(result);
    return 0;
}

static bool get_phys_addr_v5(CPUARMState *env, uint32_t address,
                             MMUAccessType access_type, ARMMMUIdx mmu_idx,
                             hwaddr *phys_ptr, int *prot,
                             target_ulong *page_size,
                             ARMMMUFaultInfo *fi)
{
    CPUState *cs = CPU(arm_env_get_cpu(env));
    int level = 1;
    uint32_t table;
    uint32_t desc;
    int type;
    int ap;
    int domain = 0;
    int domain_prot;
    hwaddr phys_addr;
    uint32_t dacr;

    /* Pagetable walk.  */
    /* Lookup l1 descriptor.  */
    if (!get_level1_table_address(env, mmu_idx, &table, address)) {
        /* Section translation fault if page walk is disabled by PD0 or PD1 */
        fi->type = ARMFault_Translation;
        goto do_fault;
    }
    desc = arm_ldl_ptw(cs, table, regime_is_secure(env, mmu_idx),
                       mmu_idx, fi);
    if (fi->type != ARMFault_None) {
        goto do_fault;
    }
    type = (desc & 3);
    domain = (desc >> 5) & 0x0f;
    if (regime_el(env, mmu_idx) == 1) {
        dacr = env->cp15.dacr_ns;
    } else {
        dacr = env->cp15.dacr_s;
    }
    domain_prot = (dacr >> (domain * 2)) & 3;
    if (type == 0) {
        /* Section translation fault.  */
        fi->type = ARMFault_Translation;
        goto do_fault;
    }
    if (type != 2) {
        level = 2;
    }
    if (domain_prot == 0 || domain_prot == 2) {
        fi->type = ARMFault_Domain;
        goto do_fault;
    }
    if (type == 2) {
        /* 1Mb section.  */
        phys_addr = (desc & 0xfff00000) | (address & 0x000fffff);
        ap = (desc >> 10) & 3;
        *page_size = 1024 * 1024;
    } else {
        /* Lookup l2 entry.  */
        if (type == 1) {
            /* Coarse pagetable.  */
            table = (desc & 0xfffffc00) | ((address >> 10) & 0x3fc);
        } else {
            /* Fine pagetable.  */
            table = (desc & 0xfffff000) | ((address >> 8) & 0xffc);
        }
        desc = arm_ldl_ptw(cs, table, regime_is_secure(env, mmu_idx),
                           mmu_idx, fi);
        if (fi->type != ARMFault_None) {
            goto do_fault;
        }
        switch (desc & 3) {
        case 0: /* Page translation fault.  */
            fi->type = ARMFault_Translation;
            goto do_fault;
        case 1: /* 64k page.  */
            phys_addr = (desc & 0xffff0000) | (address & 0xffff);
            ap = (desc >> (4 + ((address >> 13) & 6))) & 3;
            *page_size = 0x10000;
            break;
        case 2: /* 4k page.  */
            phys_addr = (desc & 0xfffff000) | (address & 0xfff);
            ap = (desc >> (4 + ((address >> 9) & 6))) & 3;
            *page_size = 0x1000;
            break;
        case 3: /* 1k page, or ARMv6/XScale "extended small (4k) page" */
            if (type == 1) {
                /* ARMv6/XScale extended small page format */
                if (arm_feature(env, ARM_FEATURE_XSCALE)
                    || arm_feature(env, ARM_FEATURE_V6)) {
                    phys_addr = (desc & 0xfffff000) | (address & 0xfff);
                    *page_size = 0x1000;
                } else {
                    /* UNPREDICTABLE in ARMv5; we choose to take a
                     * page translation fault.
                     */
                    fi->type = ARMFault_Translation;
                    goto do_fault;
                }
            } else {
                phys_addr = (desc & 0xfffffc00) | (address & 0x3ff);
                *page_size = 0x400;
            }
            ap = (desc >> 4) & 3;
            break;
        default:
            /* Never happens, but compiler isn't smart enough to tell.  */
            abort();
        }
    }
    *prot = ap_to_rw_prot(env, mmu_idx, ap, domain_prot);
    *prot |= *prot ? PAGE_EXEC : 0;
    if (!(*prot & (1 << access_type))) {
        /* Access permission fault.  */
        fi->type = ARMFault_Permission;
        goto do_fault;
    }
    *phys_ptr = phys_addr;
    return false;
do_fault:
    fi->domain = domain;
    fi->level = level;
    return true;
}

static bool get_phys_addr_v6(CPUARMState *env, uint32_t address,
                             MMUAccessType access_type, ARMMMUIdx mmu_idx,
                             hwaddr *phys_ptr, MemTxAttrs *attrs, int *prot,
                             target_ulong *page_size, ARMMMUFaultInfo *fi)
{
    CPUState *cs = CPU(arm_env_get_cpu(env));
    int level = 1;
    uint32_t table;
    uint32_t desc;
    uint32_t xn;
    uint32_t pxn = 0;
    int type;
    int ap;
    int domain = 0;
    int domain_prot;
    hwaddr phys_addr;
    uint32_t dacr;
    bool ns;

    /* Pagetable walk.  */
    /* Lookup l1 descriptor.  */
    if (!get_level1_table_address(env, mmu_idx, &table, address)) {
        /* Section translation fault if page walk is disabled by PD0 or PD1 */
        fi->type = ARMFault_Translation;
        goto do_fault;
    }
    desc = arm_ldl_ptw(cs, table, regime_is_secure(env, mmu_idx),
                       mmu_idx, fi);
    if (fi->type != ARMFault_None) {
        goto do_fault;
    }
    type = (desc & 3);
    if (type == 0 || (type == 3 && !arm_feature(env, ARM_FEATURE_PXN))) {
        /* Section translation fault, or attempt to use the encoding
         * which is Reserved on implementations without PXN.
         */
        fi->type = ARMFault_Translation;
        goto do_fault;
    }
    if ((type == 1) || !(desc & (1 << 18))) {
        /* Page or Section.  */
        domain = (desc >> 5) & 0x0f;
    }
    if (regime_el(env, mmu_idx) == 1) {
        dacr = env->cp15.dacr_ns;
    } else {
        dacr = env->cp15.dacr_s;
    }
    if (type == 1) {
        level = 2;
    }
    domain_prot = (dacr >> (domain * 2)) & 3;
    if (domain_prot == 0 || domain_prot == 2) {
        /* Section or Page domain fault */
        fi->type = ARMFault_Domain;
        goto do_fault;
    }
    if (type != 1) {
        if (desc & (1 << 18)) {
            /* Supersection.  */
            phys_addr = (desc & 0xff000000) | (address & 0x00ffffff);
            phys_addr |= (uint64_t)extract32(desc, 20, 4) << 32;
            phys_addr |= (uint64_t)extract32(desc, 5, 4) << 36;
            *page_size = 0x1000000;
        } else {
            /* Section.  */
            phys_addr = (desc & 0xfff00000) | (address & 0x000fffff);
            *page_size = 0x100000;
        }
        ap = ((desc >> 10) & 3) | ((desc >> 13) & 4);
        xn = desc & (1 << 4);
        pxn = desc & 1;
        ns = extract32(desc, 19, 1);
    } else {
        if (arm_feature(env, ARM_FEATURE_PXN)) {
            pxn = (desc >> 2) & 1;
        }
        ns = extract32(desc, 3, 1);
        /* Lookup l2 entry.  */
        table = (desc & 0xfffffc00) | ((address >> 10) & 0x3fc);
        desc = arm_ldl_ptw(cs, table, regime_is_secure(env, mmu_idx),
                           mmu_idx, fi);
        if (fi->type != ARMFault_None) {
            goto do_fault;
        }
        ap = ((desc >> 4) & 3) | ((desc >> 7) & 4);
        switch (desc & 3) {
        case 0: /* Page translation fault.  */
            fi->type = ARMFault_Translation;
            goto do_fault;
        case 1: /* 64k page.  */
            phys_addr = (desc & 0xffff0000) | (address & 0xffff);
            xn = desc & (1 << 15);
            *page_size = 0x10000;
            break;
        case 2: case 3: /* 4k page.  */
            phys_addr = (desc & 0xfffff000) | (address & 0xfff);
            xn = desc & 1;
            *page_size = 0x1000;
            break;
        default:
            /* Never happens, but compiler isn't smart enough to tell.  */
            abort();
        }
    }
    if (domain_prot == 3) {
        *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
    } else {
        if (pxn && !regime_is_user(env, mmu_idx)) {
            xn = 1;
        }
        if (xn && access_type == MMU_INST_FETCH) {
            fi->type = ARMFault_Permission;
            goto do_fault;
        }

        if (arm_feature(env, ARM_FEATURE_V6K) &&
                (regime_sctlr(env, mmu_idx) & SCTLR_AFE)) {
            /* The simplified model uses AP[0] as an access control bit.  */
            if ((ap & 1) == 0) {
                /* Access flag fault.  */
                fi->type = ARMFault_AccessFlag;
                goto do_fault;
            }
            *prot = simple_ap_to_rw_prot(env, mmu_idx, ap >> 1);
        } else {
            *prot = ap_to_rw_prot(env, mmu_idx, ap, domain_prot);
        }
        if (*prot && !xn) {
            *prot |= PAGE_EXEC;
        }
        if (!(*prot & (1 << access_type))) {
            /* Access permission fault.  */
            fi->type = ARMFault_Permission;
            goto do_fault;
        }
    }
    if (ns) {
        /* The NS bit will (as required by the architecture) have no effect if
         * the CPU doesn't support TZ or this is a non-secure translation
         * regime, because the attribute will already be non-secure.
         */
        attrs->secure = false;
    }
    *phys_ptr = phys_addr;
    return false;
do_fault:
    fi->domain = domain;
    fi->level = level;
    return true;
}

/*
 * check_s2_mmu_setup
 * @cpu:        ARMCPU
 * @is_aa64:    True if the translation regime is in AArch64 state
 * @startlevel: Suggested starting level
 * @inputsize:  Bitsize of IPAs
 * @stride:     Page-table stride (See the ARM ARM)
 *
 * Returns true if the suggested S2 translation parameters are OK and
 * false otherwise.
 */
static bool check_s2_mmu_setup(ARMCPU *cpu, bool is_aa64, int level,
                               int inputsize, int stride)
{
    const int grainsize = stride + 3;
    int startsizecheck;

    /* Negative levels are never allowed.  */
    if (level < 0) {
        return false;
    }

    startsizecheck = inputsize - ((3 - level) * stride + grainsize);
    if (startsizecheck < 1 || startsizecheck > stride + 4) {
        return false;
    }

    if (is_aa64) {
        CPUARMState *env = &cpu->env;
        unsigned int pamax = arm_pamax(cpu);

        switch (stride) {
        case 13: /* 64KB Pages.  */
            if (level == 0 || (level == 1 && pamax <= 42)) {
                return false;
            }
            break;
        case 11: /* 16KB Pages.  */
            if (level == 0 || (level == 1 && pamax <= 40)) {
                return false;
            }
            break;
        case 9: /* 4KB Pages.  */
            if (level == 0 && pamax <= 42) {
                return false;
            }
            break;
        default:
            g_assert_not_reached();
        }

        /* Inputsize checks.  */
        if (inputsize > pamax &&
            (arm_el_is_aa64(env, 1) || inputsize > 40)) {
            /* This is CONSTRAINED UNPREDICTABLE and we choose to fault.  */
            return false;
        }
    } else {
        /* AArch32 only supports 4KB pages. Assert on that.  */
        assert(stride == 9);

        if (level == 0) {
            return false;
        }
    }
    return true;
}

/* Translate from the 4-bit stage 2 representation of
 * memory attributes (without cache-allocation hints) to
 * the 8-bit representation of the stage 1 MAIR registers
 * (which includes allocation hints).
 *
 * ref: shared/translation/attrs/S2AttrDecode()
 *      .../S2ConvertAttrsHints()
 */
static uint8_t convert_stage2_attrs(CPUARMState *env, uint8_t s2attrs)
{
    uint8_t hiattr = extract32(s2attrs, 2, 2);
    uint8_t loattr = extract32(s2attrs, 0, 2);
    uint8_t hihint = 0, lohint = 0;

    if (hiattr != 0) { /* normal memory */
        if ((env->cp15.hcr_el2 & HCR_CD) != 0) { /* cache disabled */
            hiattr = loattr = 1; /* non-cacheable */
        } else {
            if (hiattr != 1) { /* Write-through or write-back */
                hihint = 3; /* RW allocate */
            }
            if (loattr != 1) { /* Write-through or write-back */
                lohint = 3; /* RW allocate */
            }
        }
    }

    return (hiattr << 6) | (hihint << 4) | (loattr << 2) | lohint;
}

static bool get_phys_addr_lpae(CPUARMState *env, target_ulong address,
                               MMUAccessType access_type, ARMMMUIdx mmu_idx,
                               hwaddr *phys_ptr, MemTxAttrs *txattrs, int *prot,
                               target_ulong *page_size_ptr,
                               ARMMMUFaultInfo *fi, ARMCacheAttrs *cacheattrs)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    CPUState *cs = CPU(cpu);
    /* Read an LPAE long-descriptor translation table. */
    ARMFaultType fault_type = ARMFault_Translation;
    uint32_t level;
    uint32_t epd = 0;
    int32_t t0sz, t1sz;
    uint32_t tg;
    uint64_t ttbr;
    int ttbr_select;
    hwaddr descaddr, indexmask, indexmask_grainsize;
    uint32_t tableattrs;
    target_ulong page_size;
    uint32_t attrs;
    int32_t stride = 9;
    int32_t addrsize;
    int inputsize;
    int32_t tbi = 0;
    TCR *tcr = regime_tcr(env, mmu_idx);
    int ap, ns, xn, pxn;
    uint32_t el = regime_el(env, mmu_idx);
    bool ttbr1_valid = true;
    uint64_t descaddrmask;
    bool aarch64 = arm_el_is_aa64(env, el);

    /* TODO:
     * This code does not handle the different format TCR for VTCR_EL2.
     * This code also does not support shareability levels.
     * Attribute and permission bit handling should also be checked when adding
     * support for those page table walks.
     */
    if (aarch64) {
        level = 0;
        addrsize = 64;
        if (el > 1) {
            if (mmu_idx != ARMMMUIdx_S2NS) {
                tbi = extract64(tcr->raw_tcr, 20, 1);
            }
        } else {
            if (extract64(address, 55, 1)) {
                tbi = extract64(tcr->raw_tcr, 38, 1);
            } else {
                tbi = extract64(tcr->raw_tcr, 37, 1);
            }
        }
        tbi *= 8;

        /* If we are in 64-bit EL2 or EL3 then there is no TTBR1, so mark it
         * invalid.
         */
        if (el > 1) {
            ttbr1_valid = false;
        }
    } else {
        level = 1;
        addrsize = 32;
        /* There is no TTBR1 for EL2 */
        if (el == 2) {
            ttbr1_valid = false;
        }
    }

    /* Determine whether this address is in the region controlled by
     * TTBR0 or TTBR1 (or if it is in neither region and should fault).
     * This is a Non-secure PL0/1 stage 1 translation, so controlled by
     * TTBCR/TTBR0/TTBR1 in accordance with ARM ARM DDI0406C table B-32:
     */
    if (aarch64) {
        /* AArch64 translation.  */
        t0sz = extract32(tcr->raw_tcr, 0, 6);
        t0sz = MIN(t0sz, 39);
        t0sz = MAX(t0sz, 16);
    } else if (mmu_idx != ARMMMUIdx_S2NS) {
        /* AArch32 stage 1 translation.  */
        t0sz = extract32(tcr->raw_tcr, 0, 3);
    } else {
        /* AArch32 stage 2 translation.  */
        bool sext = extract32(tcr->raw_tcr, 4, 1);
        bool sign = extract32(tcr->raw_tcr, 3, 1);
        /* Address size is 40-bit for a stage 2 translation,
         * and t0sz can be negative (from -8 to 7),
         * so we need to adjust it to use the TTBR selecting logic below.
         */
        addrsize = 40;
        t0sz = sextract32(tcr->raw_tcr, 0, 4) + 8;

        /* If the sign-extend bit is not the same as t0sz[3], the result
         * is unpredictable. Flag this as a guest error.  */
        if (sign != sext) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "AArch32: VTCR.S / VTCR.T0SZ[3] mismatch\n");
        }
    }
    t1sz = extract32(tcr->raw_tcr, 16, 6);
    if (aarch64) {
        t1sz = MIN(t1sz, 39);
        t1sz = MAX(t1sz, 16);
    }
    if (t0sz && !extract64(address, addrsize - t0sz, t0sz - tbi)) {
        /* there is a ttbr0 region and we are in it (high bits all zero) */
        ttbr_select = 0;
    } else if (ttbr1_valid && t1sz &&
               !extract64(~address, addrsize - t1sz, t1sz - tbi)) {
        /* there is a ttbr1 region and we are in it (high bits all one) */
        ttbr_select = 1;
    } else if (!t0sz) {
        /* ttbr0 region is "everything not in the ttbr1 region" */
        ttbr_select = 0;
    } else if (!t1sz && ttbr1_valid) {
        /* ttbr1 region is "everything not in the ttbr0 region" */
        ttbr_select = 1;
    } else {
        /* in the gap between the two regions, this is a Translation fault */
        fault_type = ARMFault_Translation;
        goto do_fault;
    }

    /* Note that QEMU ignores shareability and cacheability attributes,
     * so we don't need to do anything with the SH, ORGN, IRGN fields
     * in the TTBCR.  Similarly, TTBCR:A1 selects whether we get the
     * ASID from TTBR0 or TTBR1, but QEMU's TLB doesn't currently
     * implement any ASID-like capability so we can ignore it (instead
     * we will always flush the TLB any time the ASID is changed).
     */
    if (ttbr_select == 0) {
        ttbr = regime_ttbr(env, mmu_idx, 0);
        if (el < 2) {
            epd = extract32(tcr->raw_tcr, 7, 1);
        }
        inputsize = addrsize - t0sz;

        tg = extract32(tcr->raw_tcr, 14, 2);
        if (tg == 1) { /* 64KB pages */
            stride = 13;
        }
        if (tg == 2) { /* 16KB pages */
            stride = 11;
        }
    } else {
        /* We should only be here if TTBR1 is valid */
        assert(ttbr1_valid);

        ttbr = regime_ttbr(env, mmu_idx, 1);
        epd = extract32(tcr->raw_tcr, 23, 1);
        inputsize = addrsize - t1sz;

        tg = extract32(tcr->raw_tcr, 30, 2);
        if (tg == 3)  { /* 64KB pages */
            stride = 13;
        }
        if (tg == 1) { /* 16KB pages */
            stride = 11;
        }
    }

    /* Here we should have set up all the parameters for the translation:
     * inputsize, ttbr, epd, stride, tbi
     */

    if (epd) {
        /* Translation table walk disabled => Translation fault on TLB miss
         * Note: This is always 0 on 64-bit EL2 and EL3.
         */
        goto do_fault;
    }

    if (mmu_idx != ARMMMUIdx_S2NS) {
        /* The starting level depends on the virtual address size (which can
         * be up to 48 bits) and the translation granule size. It indicates
         * the number of strides (stride bits at a time) needed to
         * consume the bits of the input address. In the pseudocode this is:
         *  level = 4 - RoundUp((inputsize - grainsize) / stride)
         * where their 'inputsize' is our 'inputsize', 'grainsize' is
         * our 'stride + 3' and 'stride' is our 'stride'.
         * Applying the usual "rounded up m/n is (m+n-1)/n" and simplifying:
         * = 4 - (inputsize - stride - 3 + stride - 1) / stride
         * = 4 - (inputsize - 4) / stride;
         */
        level = 4 - (inputsize - 4) / stride;
    } else {
        /* For stage 2 translations the starting level is specified by the
         * VTCR_EL2.SL0 field (whose interpretation depends on the page size)
         */
        uint32_t sl0 = extract32(tcr->raw_tcr, 6, 2);
        uint32_t startlevel;
        bool ok;

        if (!aarch64 || stride == 9) {
            /* AArch32 or 4KB pages */
            startlevel = 2 - sl0;
        } else {
            /* 16KB or 64KB pages */
            startlevel = 3 - sl0;
        }

        /* Check that the starting level is valid. */
        ok = check_s2_mmu_setup(cpu, aarch64, startlevel,
                                inputsize, stride);
        if (!ok) {
            fault_type = ARMFault_Translation;
            goto do_fault;
        }
        level = startlevel;
    }

    indexmask_grainsize = (1ULL << (stride + 3)) - 1;
    indexmask = (1ULL << (inputsize - (stride * (4 - level)))) - 1;

    /* Now we can extract the actual base address from the TTBR */
    descaddr = extract64(ttbr, 0, 48);
    descaddr &= ~indexmask;

    /* The address field in the descriptor goes up to bit 39 for ARMv7
     * but up to bit 47 for ARMv8, but we use the descaddrmask
     * up to bit 39 for AArch32, because we don't need other bits in that case
     * to construct next descriptor address (anyway they should be all zeroes).
     */
    descaddrmask = ((1ull << (aarch64 ? 48 : 40)) - 1) &
                   ~indexmask_grainsize;

    /* Secure accesses start with the page table in secure memory and
     * can be downgraded to non-secure at any step. Non-secure accesses
     * remain non-secure. We implement this by just ORing in the NSTable/NS
     * bits at each step.
     */
    tableattrs = regime_is_secure(env, mmu_idx) ? 0 : (1 << 4);
    for (;;) {
        uint64_t descriptor;
        bool nstable;

        descaddr |= (address >> (stride * (4 - level))) & indexmask;
        descaddr &= ~7ULL;
        nstable = extract32(tableattrs, 4, 1);
        descriptor = arm_ldq_ptw(cs, descaddr, !nstable, mmu_idx, fi);
        if (fi->type != ARMFault_None) {
            goto do_fault;
        }

        if (!(descriptor & 1) ||
            (!(descriptor & 2) && (level == 3))) {
            /* Invalid, or the Reserved level 3 encoding */
            goto do_fault;
        }
        descaddr = descriptor & descaddrmask;

        if ((descriptor & 2) && (level < 3)) {
            /* Table entry. The top five bits are attributes which  may
             * propagate down through lower levels of the table (and
             * which are all arranged so that 0 means "no effect", so
             * we can gather them up by ORing in the bits at each level).
             */
            tableattrs |= extract64(descriptor, 59, 5);
            level++;
            indexmask = indexmask_grainsize;
            continue;
        }
        /* Block entry at level 1 or 2, or page entry at level 3.
         * These are basically the same thing, although the number
         * of bits we pull in from the vaddr varies.
         */
        page_size = (1ULL << ((stride * (4 - level)) + 3));
        descaddr |= (address & (page_size - 1));
        /* Extract attributes from the descriptor */
        attrs = extract64(descriptor, 2, 10)
            | (extract64(descriptor, 52, 12) << 10);

        if (mmu_idx == ARMMMUIdx_S2NS) {
            /* Stage 2 table descriptors do not include any attribute fields */
            break;
        }
        /* Merge in attributes from table descriptors */
        attrs |= extract32(tableattrs, 0, 2) << 11; /* XN, PXN */
        attrs |= extract32(tableattrs, 3, 1) << 5; /* APTable[1] => AP[2] */
        /* The sense of AP[1] vs APTable[0] is reversed, as APTable[0] == 1
         * means "force PL1 access only", which means forcing AP[1] to 0.
         */
        if (extract32(tableattrs, 2, 1)) {
            attrs &= ~(1 << 4);
        }
        attrs |= nstable << 3; /* NS */
        break;
    }
    /* Here descaddr is the final physical address, and attributes
     * are all in attrs.
     */
    fault_type = ARMFault_AccessFlag;
    if ((attrs & (1 << 8)) == 0) {
        /* Access flag */
        goto do_fault;
    }

    ap = extract32(attrs, 4, 2);
    xn = extract32(attrs, 12, 1);

    if (mmu_idx == ARMMMUIdx_S2NS) {
        ns = true;
        *prot = get_S2prot(env, ap, xn);
    } else {
        ns = extract32(attrs, 3, 1);
        pxn = extract32(attrs, 11, 1);
        *prot = get_S1prot(env, mmu_idx, aarch64, ap, ns, xn, pxn);
    }

    fault_type = ARMFault_Permission;
    if (!(*prot & (1 << access_type))) {
        goto do_fault;
    }

    if (ns) {
        /* The NS bit will (as required by the architecture) have no effect if
         * the CPU doesn't support TZ or this is a non-secure translation
         * regime, because the attribute will already be non-secure.
         */
        txattrs->secure = false;
    }

    if (cacheattrs != NULL) {
        if (mmu_idx == ARMMMUIdx_S2NS) {
            cacheattrs->attrs = convert_stage2_attrs(env,
                                                     extract32(attrs, 0, 4));
        } else {
            /* Index into MAIR registers for cache attributes */
            uint8_t attrindx = extract32(attrs, 0, 3);
            uint64_t mair = env->cp15.mair_el[regime_el(env, mmu_idx)];
            assert(attrindx <= 7);
            cacheattrs->attrs = extract64(mair, attrindx * 8, 8);
        }
        cacheattrs->shareability = extract32(attrs, 6, 2);
    }

    *phys_ptr = descaddr;
    *page_size_ptr = page_size;
    return false;

do_fault:
    fi->type = fault_type;
    fi->level = level;
    /* Tag the error as S2 for failed S1 PTW at S2 or ordinary S2.  */
    fi->stage2 = fi->s1ptw || (mmu_idx == ARMMMUIdx_S2NS);
    return true;
}

static inline void get_phys_addr_pmsav7_default(CPUARMState *env,
                                                ARMMMUIdx mmu_idx,
                                                int32_t address, int *prot)
{
    if (!arm_feature(env, ARM_FEATURE_M)) {
        *prot = PAGE_READ | PAGE_WRITE;

        if (address >= 0xF0000000 && address <= 0xFFFFFFFF) {
            if (regime_sctlr(env, mmu_idx) & SCTLR_V) {
                /* hivecs execing is ok */
                *prot |= PAGE_EXEC;
            }
        } else if (address >= 0x00000000 && address <= 0x7FFFFFFF) {
            *prot |= PAGE_EXEC;
        }
    } else {
        /* Default system address map for M profile cores.
         * The architecture specifies which regions are execute-never;
         * at the MPU level no other checks are defined.
         */
        if ((address >= 0x00000000 && address <= 0x1FFFFFFF) || /* ROM */
            (address >= 0x20000000 && address <= 0x3FFFFFFF) || /* SRAM */
            (address >= 0x60000000 && address <= 0x7FFFFFFF) || /* RAM */
            (address >= 0x80000000 && address <= 0x9FFFFFFF)) { /* RAM */
            *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        } else if ((address >= 0x40000000 && address <= 0x5FFFFFFF) || /* Peripheral */
                   (address >= 0xA0000000 && address <= 0xBFFFFFFF) || /* Device */
                   (address >= 0xC0000000 && address <= 0xDFFFFFFF) || /* Device */
                   (address >= 0xE0000000 && address <= 0xFFFFFFFF)) { /* System */
            *prot = PAGE_READ | PAGE_WRITE;
        } else {
             g_assert_not_reached();
        }
    }
}

static bool pmsav7_use_background_region(ARMCPU *cpu,
                                         ARMMMUIdx mmu_idx, bool is_user)
{
    /* Return true if we should use the default memory map as a
     * "background" region if there are no hits against any MPU regions.
     */
    CPUARMState *env = &cpu->env;

    if (is_user) {
        return false;
    }

    if (arm_feature(env, ARM_FEATURE_M)) {
        return env->v7m.mpu_ctrl[regime_is_secure(env, mmu_idx)]
            & R_V7M_MPU_CTRL_PRIVDEFENA_MASK;
    } else {
        return regime_sctlr(env, mmu_idx) & SCTLR_BR;
    }
}

static inline bool m_is_ppb_region(CPUARMState *env, uint32_t address)
{
    /* True if address is in the M profile PPB region 0xe0000000 - 0xe00fffff */
    return arm_feature(env, ARM_FEATURE_M) &&
        extract32(address, 20, 12) == 0xe00;
}

static inline bool m_is_system_region(CPUARMState *env, uint32_t address)
{
    /* True if address is in the M profile system region
     * 0xe0000000 - 0xffffffff
     */
    return arm_feature(env, ARM_FEATURE_M) && extract32(address, 29, 3) == 0x7;
}

static bool get_phys_addr_pmsav7(CPUARMState *env, uint32_t address,
                                 MMUAccessType access_type, ARMMMUIdx mmu_idx,
                                 hwaddr *phys_ptr, int *prot,
                                 ARMMMUFaultInfo *fi)
{
    ARMCPU *cpu = arm_env_get_cpu(env);
    int n;
    bool is_user = regime_is_user(env, mmu_idx);

    *phys_ptr = address;
    *prot = 0;

    if (regime_translation_disabled(env, mmu_idx) ||
        m_is_ppb_region(env, address)) {
        /* MPU disabled or M profile PPB access: use default memory map.
         * The other case which uses the default memory map in the
         * v7M ARM ARM pseudocode is exception vector reads from the vector
         * table. In QEMU those accesses are done in arm_v7m_load_vector(),
         * which always does a direct read using address_space_ldl(), rather
         * than going via this function, so we don't need to check that here.
         */
        get_phys_addr_pmsav7_default(env, mmu_idx, address, prot);
    } else { /* MPU enabled */
        for (n = (int)cpu->pmsav7_dregion - 1; n >= 0; n--) {
            /* region search */
            uint32_t base = env->pmsav7.drbar[n];
            uint32_t rsize = extract32(env->pmsav7.drsr[n], 1, 5);
            uint32_t rmask;
            bool srdis = false;

            if (!(env->pmsav7.drsr[n] & 0x1)) {
                continue;
            }

            if (!rsize) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "DRSR[%d]: Rsize field cannot be 0\n", n);
                continue;
            }
            rsize++;
            rmask = (1ull << rsize) - 1;

            if (base & rmask) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "DRBAR[%d]: 0x%" PRIx32 " misaligned "
                              "to DRSR region size, mask = 0x%" PRIx32 "\n",
                              n, base, rmask);
                continue;
            }

            if (address < base || address > base + rmask) {
                continue;
            }

            /* Region matched */

            if (rsize >= 8) { /* no subregions for regions < 256 bytes */
                int i, snd;
                uint32_t srdis_mask;

                rsize -= 3; /* sub region size (power of 2) */
                snd = ((address - base) >> rsize) & 0x7;
                srdis = extract32(env->pmsav7.drsr[n], snd + 8, 1);

                srdis_mask = srdis ? 0x3 : 0x0;
                for (i = 2; i <= 8 && rsize < TARGET_PAGE_BITS; i *= 2) {
                    /* This will check in groups of 2, 4 and then 8, whether
                     * the subregion bits are consistent. rsize is incremented
                     * back up to give the region size, considering consistent
                     * adjacent subregions as one region. Stop testing if rsize
                     * is already big enough for an entire QEMU page.
                     */
                    int snd_rounded = snd & ~(i - 1);
                    uint32_t srdis_multi = extract32(env->pmsav7.drsr[n],
                                                     snd_rounded + 8, i);
                    if (srdis_mask ^ srdis_multi) {
                        break;
                    }
                    srdis_mask = (srdis_mask << i) | srdis_mask;
                    rsize++;
                }
            }
            if (rsize < TARGET_PAGE_BITS) {
                qemu_log_mask(LOG_UNIMP,
                              "DRSR[%d]: No support for MPU (sub)region "
                              "alignment of %" PRIu32 " bits. Minimum is %d\n",
                              n, rsize, TARGET_PAGE_BITS);
                continue;
            }
            if (srdis) {
                continue;
            }
            break;
        }

        if (n == -1) { /* no hits */
            if (!pmsav7_use_background_region(cpu, mmu_idx, is_user)) {
                /* background fault */
                fi->type = ARMFault_Background;
                return true;
            }
            get_phys_addr_pmsav7_default(env, mmu_idx, address, prot);
        } else { /* a MPU hit! */
            uint32_t ap = extract32(env->pmsav7.dracr[n], 8, 3);
            uint32_t xn = extract32(env->pmsav7.dracr[n], 12, 1);

            if (m_is_system_region(env, address)) {
                /* System space is always execute never */
                xn = 1;
            }

            if (is_user) { /* User mode AP bit decoding */
                switch (ap) {
                case 0:
                case 1:
                case 5:
                    break; /* no access */
                case 3:
                    *prot |= PAGE_WRITE;
                    /* fall through */
                case 2:
                case 6:
                    *prot |= PAGE_READ | PAGE_EXEC;
                    break;
                case 7:
                    /* for v7M, same as 6; for R profile a reserved value */
                    if (arm_feature(env, ARM_FEATURE_M)) {
                        *prot |= PAGE_READ | PAGE_EXEC;
                        break;
                    }
                    /* fall through */
                default:
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "DRACR[%d]: Bad value for AP bits: 0x%"
                                  PRIx32 "\n", n, ap);
                }
            } else { /* Priv. mode AP bits decoding */
                switch (ap) {
                case 0:
                    break; /* no access */
                case 1:
                case 2:
                case 3:
                    *prot |= PAGE_WRITE;
                    /* fall through */
                case 5:
                case 6:
                    *prot |= PAGE_READ | PAGE_EXEC;
                    break;
                case 7:
                    /* for v7M, same as 6; for R profile a reserved value */
                    if (arm_feature(env, ARM_FEATURE_M)) {
                        *prot |= PAGE_READ | PAGE_EXEC;
                        break;
                    }
                    /* fall through */
                default:
                    qemu_log_mask(LOG_GUEST_ERROR,
                                  "DRACR[%d]: Bad value for AP bits: 0x%"
                                  PRIx32 "\n", n, ap);
                }
            }

            /* execute never */
            if (xn) {
                *prot &= ~PAGE_EXEC;
            }
        }
    }

    fi->type = ARMFault_Permission;
    fi->level = 1;
    return !(*prot & (1 << access_type));
}

static bool v8m_is_sau_exempt(CPUARMState *env,
                              uint32_t address, MMUAccessType access_type)
{
    /* The architecture specifies that certain address ranges are
     * exempt from v8M SAU/IDAU checks.
     */
    return
        (access_type == MMU_INST_FETCH && m_is_system_region(env, address)) ||
        (address >= 0xe0000000 && address <= 0xe0002fff) ||
        (address >= 0xe000e000 && address <= 0xe000efff) ||
        (address >= 0xe002e000 && address <= 0xe002efff) ||
        (address >= 0xe0040000 && address <= 0xe0041fff) ||
        (address >= 0xe00ff000 && address <= 0xe00fffff);
}

static void v8m_security_lookup(CPUARMState *env, uint32_t address,
                                MMUAccessType access_type, ARMMMUIdx mmu_idx,
                                V8M_SAttributes *sattrs)
{
    /* Look up the security attributes for this address. Compare the
     * pseudocode SecurityCheck() function.
     * We assume the caller has zero-initialized *sattrs.
     */
    ARMCPU *cpu = arm_env_get_cpu(env);
    int r;

    /* TODO: implement IDAU */

    if (access_type == MMU_INST_FETCH && extract32(address, 28, 4) == 0xf) {
        /* 0xf0000000..0xffffffff is always S for insn fetches */
        return;
    }

    if (v8m_is_sau_exempt(env, address, access_type)) {
        sattrs->ns = !regime_is_secure(env, mmu_idx);
        return;
    }

    switch (env->sau.ctrl & 3) {
    case 0: /* SAU.ENABLE == 0, SAU.ALLNS == 0 */
        break;
    case 2: /* SAU.ENABLE == 0, SAU.ALLNS == 1 */
        sattrs->ns = true;
        break;
    default: /* SAU.ENABLE == 1 */
        for (r = 0; r < cpu->sau_sregion; r++) {
            if (env->sau.rlar[r] & 1) {
                uint32_t base = env->sau.rbar[r] & ~0x1f;
                uint32_t limit = env->sau.rlar[r] | 0x1f;

                if (base <= address && limit >= address) {
                    if (sattrs->srvalid) {
                        /* If we hit in more than one region then we must report
                         * as Secure, not NS-Callable, with no valid region
                         * number info.
                         */
                        sattrs->ns = false;
                        sattrs->nsc = false;
                        sattrs->sregion = 0;
                        sattrs->srvalid = false;
                        break;
                    } else {
                        if (env->sau.rlar[r] & 2) {
                            sattrs->nsc = true;
                        } else {
                            sattrs->ns = true;
                        }
                        sattrs->srvalid = true;
                        sattrs->sregion = r;
                    }
                }
            }
        }

        /* TODO when we support the IDAU then it may override the result here */
        break;
    }
}

static bool pmsav8_mpu_lookup(CPUARMState *env, uint32_t address,
                              MMUAccessType access_type, ARMMMUIdx mmu_idx,
                              hwaddr *phys_ptr, MemTxAttrs *txattrs,
                              int *prot, ARMMMUFaultInfo *fi, uint32_t *mregion)
{
    /* Perform a PMSAv8 MPU lookup (without also doing the SAU check
     * that a full phys-to-virt translation does).
     * mregion is (if not NULL) set to the region number which matched,
     * or -1 if no region number is returned (MPU off, address did not
     * hit a region, address hit in multiple regions).
     */
    ARMCPU *cpu = arm_env_get_cpu(env);
    bool is_user = regime_is_user(env, mmu_idx);
    uint32_t secure = regime_is_secure(env, mmu_idx);
    int n;
    int matchregion = -1;
    bool hit = false;

    *phys_ptr = address;
    *prot = 0;
    if (mregion) {
        *mregion = -1;
    }

    /* Unlike the ARM ARM pseudocode, we don't need to check whether this
     * was an exception vector read from the vector table (which is always
     * done using the default system address map), because those accesses
     * are done in arm_v7m_load_vector(), which always does a direct
     * read using address_space_ldl(), rather than going via this function.
     */
    if (regime_translation_disabled(env, mmu_idx)) { /* MPU disabled */
        hit = true;
    } else if (m_is_ppb_region(env, address)) {
        hit = true;
    } else if (pmsav7_use_background_region(cpu, mmu_idx, is_user)) {
        hit = true;
    } else {
        for (n = (int)cpu->pmsav7_dregion - 1; n >= 0; n--) {
            /* region search */
            /* Note that the base address is bits [31:5] from the register
             * with bits [4:0] all zeroes, but the limit address is bits
             * [31:5] from the register with bits [4:0] all ones.
             */
            uint32_t base = env->pmsav8.rbar[secure][n] & ~0x1f;
            uint32_t limit = env->pmsav8.rlar[secure][n] | 0x1f;

            if (!(env->pmsav8.rlar[secure][n] & 0x1)) {
                /* Region disabled */
                continue;
            }

            if (address < base || address > limit) {
                continue;
            }

            if (hit) {
                /* Multiple regions match -- always a failure (unlike
                 * PMSAv7 where highest-numbered-region wins)
                 */
                fi->type = ARMFault_Permission;
                fi->level = 1;
                return true;
            }

            matchregion = n;
            hit = true;

            if (base & ~TARGET_PAGE_MASK) {
                qemu_log_mask(LOG_UNIMP,
                              "MPU_RBAR[%d]: No support for MPU region base"
                              "address of 0x%" PRIx32 ". Minimum alignment is "
                              "%d\n",
                              n, base, TARGET_PAGE_BITS);
                continue;
            }
            if ((limit + 1) & ~TARGET_PAGE_MASK) {
                qemu_log_mask(LOG_UNIMP,
                              "MPU_RBAR[%d]: No support for MPU region limit"
                              "address of 0x%" PRIx32 ". Minimum alignment is "
                              "%d\n",
                              n, limit, TARGET_PAGE_BITS);
                continue;
            }
        }
    }

    if (!hit) {
        /* background fault */
        fi->type = ARMFault_Background;
        return true;
    }

    if (matchregion == -1) {
        /* hit using the background region */
        get_phys_addr_pmsav7_default(env, mmu_idx, address, prot);
    } else {
        uint32_t ap = extract32(env->pmsav8.rbar[secure][matchregion], 1, 2);
        uint32_t xn = extract32(env->pmsav8.rbar[secure][matchregion], 0, 1);

        if (m_is_system_region(env, address)) {
            /* System space is always execute never */
            xn = 1;
        }

        *prot = simple_ap_to_rw_prot(env, mmu_idx, ap);
        if (*prot && !xn) {
            *prot |= PAGE_EXEC;
        }
        /* We don't need to look the attribute up in the MAIR0/MAIR1
         * registers because that only tells us about cacheability.
         */
        if (mregion) {
            *mregion = matchregion;
        }
    }

    fi->type = ARMFault_Permission;
    fi->level = 1;
    return !(*prot & (1 << access_type));
}


static bool get_phys_addr_pmsav8(CPUARMState *env, uint32_t address,
                                 MMUAccessType access_type, ARMMMUIdx mmu_idx,
                                 hwaddr *phys_ptr, MemTxAttrs *txattrs,
                                 int *prot, ARMMMUFaultInfo *fi)
{
    uint32_t secure = regime_is_secure(env, mmu_idx);
    V8M_SAttributes sattrs = {0};

    if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
        v8m_security_lookup(env, address, access_type, mmu_idx, &sattrs);
        if (access_type == MMU_INST_FETCH) {
            /* Instruction fetches always use the MMU bank and the
             * transaction attribute determined by the fetch address,
             * regardless of CPU state. This is painful for QEMU
             * to handle, because it would mean we need to encode
             * into the mmu_idx not just the (user, negpri) information
             * for the current security state but also that for the
             * other security state, which would balloon the number
             * of mmu_idx values needed alarmingly.
             * Fortunately we can avoid this because it's not actually
             * possible to arbitrarily execute code from memory with
             * the wrong security attribute: it will always generate
             * an exception of some kind or another, apart from the
             * special case of an NS CPU executing an SG instruction
             * in S&NSC memory. So we always just fail the translation
             * here and sort things out in the exception handler
             * (including possibly emulating an SG instruction).
             */
            if (sattrs.ns != !secure) {
                if (sattrs.nsc) {
                    fi->type = ARMFault_QEMU_NSCExec;
                } else {
                    fi->type = ARMFault_QEMU_SFault;
                }
                *phys_ptr = address;
                *prot = 0;
                return true;
            }
        } else {
            /* For data accesses we always use the MMU bank indicated
             * by the current CPU state, but the security attributes
             * might downgrade a secure access to nonsecure.
             */
            if (sattrs.ns) {
                txattrs->secure = false;
            } else if (!secure) {
                /* NS access to S memory must fault.
                 * Architecturally we should first check whether the
                 * MPU information for this address indicates that we
                 * are doing an unaligned access to Device memory, which
                 * should generate a UsageFault instead. QEMU does not
                 * currently check for that kind of unaligned access though.
                 * If we added it we would need to do so as a special case
                 * for M_FAKE_FSR_SFAULT in arm_v7m_cpu_do_interrupt().
                 */
                fi->type = ARMFault_QEMU_SFault;
                *phys_ptr = address;
                *prot = 0;
                return true;
            }
        }
    }

    return pmsav8_mpu_lookup(env, address, access_type, mmu_idx, phys_ptr,
                             txattrs, prot, fi, NULL);
}

static bool get_phys_addr_pmsav5(CPUARMState *env, uint32_t address,
                                 MMUAccessType access_type, ARMMMUIdx mmu_idx,
                                 hwaddr *phys_ptr, int *prot,
                                 ARMMMUFaultInfo *fi)
{
    int n;
    uint32_t mask;
    uint32_t base;
    bool is_user = regime_is_user(env, mmu_idx);

    if (regime_translation_disabled(env, mmu_idx)) {
        /* MPU disabled.  */
        *phys_ptr = address;
        *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        return false;
    }

    *phys_ptr = address;
    for (n = 7; n >= 0; n--) {
        base = env->cp15.c6_region[n];
        if ((base & 1) == 0) {
            continue;
        }
        mask = 1 << ((base >> 1) & 0x1f);
        /* Keep this shift separate from the above to avoid an
           (undefined) << 32.  */
        mask = (mask << 1) - 1;
        if (((base ^ address) & ~mask) == 0) {
            break;
        }
    }
    if (n < 0) {
        fi->type = ARMFault_Background;
        return true;
    }

    if (access_type == MMU_INST_FETCH) {
        mask = env->cp15.pmsav5_insn_ap;
    } else {
        mask = env->cp15.pmsav5_data_ap;
    }
    mask = (mask >> (n * 4)) & 0xf;
    switch (mask) {
    case 0:
        fi->type = ARMFault_Permission;
        fi->level = 1;
        return true;
    case 1:
        if (is_user) {
            fi->type = ARMFault_Permission;
            fi->level = 1;
            return true;
        }
        *prot = PAGE_READ | PAGE_WRITE;
        break;
    case 2:
        *prot = PAGE_READ;
        if (!is_user) {
            *prot |= PAGE_WRITE;
        }
        break;
    case 3:
        *prot = PAGE_READ | PAGE_WRITE;
        break;
    case 5:
        if (is_user) {
            fi->type = ARMFault_Permission;
            fi->level = 1;
            return true;
        }
        *prot = PAGE_READ;
        break;
    case 6:
        *prot = PAGE_READ;
        break;
    default:
        /* Bad permission.  */
        fi->type = ARMFault_Permission;
        fi->level = 1;
        return true;
    }
    *prot |= PAGE_EXEC;
    return false;
}

/* Combine either inner or outer cacheability attributes for normal
 * memory, according to table D4-42 and pseudocode procedure
 * CombineS1S2AttrHints() of ARM DDI 0487B.b (the ARMv8 ARM).
 *
 * NB: only stage 1 includes allocation hints (RW bits), leading to
 * some asymmetry.
 */
static uint8_t combine_cacheattr_nibble(uint8_t s1, uint8_t s2)
{
    if (s1 == 4 || s2 == 4) {
        /* non-cacheable has precedence */
        return 4;
    } else if (extract32(s1, 2, 2) == 0 || extract32(s1, 2, 2) == 2) {
        /* stage 1 write-through takes precedence */
        return s1;
    } else if (extract32(s2, 2, 2) == 2) {
        /* stage 2 write-through takes precedence, but the allocation hint
         * is still taken from stage 1
         */
        return (2 << 2) | extract32(s1, 0, 2);
    } else { /* write-back */
        return s1;
    }
}

/* Combine S1 and S2 cacheability/shareability attributes, per D4.5.4
 * and CombineS1S2Desc()
 *
 * @s1:      Attributes from stage 1 walk
 * @s2:      Attributes from stage 2 walk
 */
static ARMCacheAttrs combine_cacheattrs(ARMCacheAttrs s1, ARMCacheAttrs s2)
{
    uint8_t s1lo = extract32(s1.attrs, 0, 4), s2lo = extract32(s2.attrs, 0, 4);
    uint8_t s1hi = extract32(s1.attrs, 4, 4), s2hi = extract32(s2.attrs, 4, 4);
    ARMCacheAttrs ret;

    /* Combine shareability attributes (table D4-43) */
    if (s1.shareability == 2 || s2.shareability == 2) {
        /* if either are outer-shareable, the result is outer-shareable */
        ret.shareability = 2;
    } else if (s1.shareability == 3 || s2.shareability == 3) {
        /* if either are inner-shareable, the result is inner-shareable */
        ret.shareability = 3;
    } else {
        /* both non-shareable */
        ret.shareability = 0;
    }

    /* Combine memory type and cacheability attributes */
    if (s1hi == 0 || s2hi == 0) {
        /* Device has precedence over normal */
        if (s1lo == 0 || s2lo == 0) {
            /* nGnRnE has precedence over anything */
            ret.attrs = 0;
        } else if (s1lo == 4 || s2lo == 4) {
            /* non-Reordering has precedence over Reordering */
            ret.attrs = 4;  /* nGnRE */
        } else if (s1lo == 8 || s2lo == 8) {
            /* non-Gathering has precedence over Gathering */
            ret.attrs = 8;  /* nGRE */
        } else {
            ret.attrs = 0xc; /* GRE */
        }

        /* Any location for which the resultant memory type is any
         * type of Device memory is always treated as Outer Shareable.
         */
        ret.shareability = 2;
    } else { /* Normal memory */
        /* Outer/inner cacheability combine independently */
        ret.attrs = combine_cacheattr_nibble(s1hi, s2hi) << 4
                  | combine_cacheattr_nibble(s1lo, s2lo);

        if (ret.attrs == 0x44) {
            /* Any location for which the resultant memory type is Normal
             * Inner Non-cacheable, Outer Non-cacheable is always treated
             * as Outer Shareable.
             */
            ret.shareability = 2;
        }
    }

    return ret;
}


/* get_phys_addr - get the physical address for this virtual address
 *
 * Find the physical address corresponding to the given virtual address,
 * by doing a translation table walk on MMU based systems or using the
 * MPU state on MPU based systems.
 *
 * Returns false if the translation was successful. Otherwise, phys_ptr, attrs,
 * prot and page_size may not be filled in, and the populated fsr value provides
 * information on why the translation aborted, in the format of a
 * DFSR/IFSR fault register, with the following caveats:
 *  * we honour the short vs long DFSR format differences.
 *  * the WnR bit is never set (the caller must do this).
 *  * for PSMAv5 based systems we don't bother to return a full FSR format
 *    value.
 *
 * @env: CPUARMState
 * @address: virtual address to get physical address for
 * @access_type: 0 for read, 1 for write, 2 for execute
 * @mmu_idx: MMU index indicating required translation regime
 * @phys_ptr: set to the physical address corresponding to the virtual address
 * @attrs: set to the memory transaction attributes to use
 * @prot: set to the permissions for the page containing phys_ptr
 * @page_size: set to the size of the page containing phys_ptr
 * @fi: set to fault info if the translation fails
 * @cacheattrs: (if non-NULL) set to the cacheability/shareability attributes
 */
static bool get_phys_addr(CPUARMState *env, target_ulong address,
                          MMUAccessType access_type, ARMMMUIdx mmu_idx,
                          hwaddr *phys_ptr, MemTxAttrs *attrs, int *prot,
                          target_ulong *page_size,
                          ARMMMUFaultInfo *fi, ARMCacheAttrs *cacheattrs)
{
    if (mmu_idx == ARMMMUIdx_S12NSE0 || mmu_idx == ARMMMUIdx_S12NSE1) {
        /* Call ourselves recursively to do the stage 1 and then stage 2
         * translations.
         */
        if (arm_feature(env, ARM_FEATURE_EL2)) {
            hwaddr ipa;
            int s2_prot;
            int ret;
            ARMCacheAttrs cacheattrs2 = {0};

            ret = get_phys_addr(env, address, access_type,
                                stage_1_mmu_idx(mmu_idx), &ipa, attrs,
                                prot, page_size, fi, cacheattrs);

            /* If S1 fails or S2 is disabled, return early.  */
            if (ret || regime_translation_disabled(env, ARMMMUIdx_S2NS)) {
                *phys_ptr = ipa;
                return ret;
            }

            /* S1 is done. Now do S2 translation.  */
            ret = get_phys_addr_lpae(env, ipa, access_type, ARMMMUIdx_S2NS,
                                     phys_ptr, attrs, &s2_prot,
                                     page_size, fi,
                                     cacheattrs != NULL ? &cacheattrs2 : NULL);
            fi->s2addr = ipa;
            /* Combine the S1 and S2 perms.  */
            *prot &= s2_prot;

            /* Combine the S1 and S2 cache attributes, if needed */
            if (!ret && cacheattrs != NULL) {
                *cacheattrs = combine_cacheattrs(*cacheattrs, cacheattrs2);
            }

            return ret;
        } else {
            /*
             * For non-EL2 CPUs a stage1+stage2 translation is just stage 1.
             */
            mmu_idx = stage_1_mmu_idx(mmu_idx);
        }
    }

    /* The page table entries may downgrade secure to non-secure, but
     * cannot upgrade an non-secure translation regime's attributes
     * to secure.
     */
    attrs->secure = regime_is_secure(env, mmu_idx);
    attrs->user = regime_is_user(env, mmu_idx);

    /* Fast Context Switch Extension. This doesn't exist at all in v8.
     * In v7 and earlier it affects all stage 1 translations.
     */
    if (address < 0x02000000 && mmu_idx != ARMMMUIdx_S2NS
        && !arm_feature(env, ARM_FEATURE_V8)) {
        if (regime_el(env, mmu_idx) == 3) {
            address += env->cp15.fcseidr_s;
        } else {
            address += env->cp15.fcseidr_ns;
        }
    }

    if (arm_feature(env, ARM_FEATURE_PMSA)) {
        bool ret;
        *page_size = TARGET_PAGE_SIZE;

        if (arm_feature(env, ARM_FEATURE_V8)) {
            /* PMSAv8 */
            ret = get_phys_addr_pmsav8(env, address, access_type, mmu_idx,
                                       phys_ptr, attrs, prot, fi);
        } else if (arm_feature(env, ARM_FEATURE_V7)) {
            /* PMSAv7 */
            ret = get_phys_addr_pmsav7(env, address, access_type, mmu_idx,
                                       phys_ptr, prot, fi);
        } else {
            /* Pre-v7 MPU */
            ret = get_phys_addr_pmsav5(env, address, access_type, mmu_idx,
                                       phys_ptr, prot, fi);
        }
        qemu_log_mask(CPU_LOG_MMU, "PMSA MPU lookup for %s at 0x%08" PRIx32
                      " mmu_idx %u -> %s (prot %c%c%c)\n",
                      access_type == MMU_DATA_LOAD ? "reading" :
                      (access_type == MMU_DATA_STORE ? "writing" : "execute"),
                      (uint32_t)address, mmu_idx,
                      ret ? "Miss" : "Hit",
                      *prot & PAGE_READ ? 'r' : '-',
                      *prot & PAGE_WRITE ? 'w' : '-',
                      *prot & PAGE_EXEC ? 'x' : '-');

        return ret;
    }

    /* Definitely a real MMU, not an MPU */

    if (regime_translation_disabled(env, mmu_idx)) {
        /* MMU disabled. */
        *phys_ptr = address;
        *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        *page_size = TARGET_PAGE_SIZE;
        return 0;
    }

    if (regime_using_lpae_format(env, mmu_idx)) {
        return get_phys_addr_lpae(env, address, access_type, mmu_idx,
                                  phys_ptr, attrs, prot, page_size,
                                  fi, cacheattrs);
    } else if (regime_sctlr(env, mmu_idx) & SCTLR_XP) {
        return get_phys_addr_v6(env, address, access_type, mmu_idx,
                                phys_ptr, attrs, prot, page_size, fi);
    } else {
        return get_phys_addr_v5(env, address, access_type, mmu_idx,
                                    phys_ptr, prot, page_size, fi);
    }
}

/* Walk the page table and (if the mapping exists) add the page
 * to the TLB. Return false on success, or true on failure. Populate
 * fsr with ARM DFSR/IFSR fault register format value on failure.
 */
bool arm_tlb_fill(CPUState *cs, vaddr address,
                  MMUAccessType access_type, int mmu_idx,
                  ARMMMUFaultInfo *fi)
{
    CPUARMState *env = cs->env_ptr;
    hwaddr phys_addr;
    target_ulong page_size;
    int prot;
    int ret;
    MemTxAttrs attrs = {0};

    ret = get_phys_addr(env, address, access_type,
                        core_to_arm_mmu_idx(env, mmu_idx), &phys_addr,
                        &attrs, &prot, &page_size, fi, NULL);
    if (!ret) {
        /* Map a single [sub]page.  */
        phys_addr &= TARGET_PAGE_MASK;
        address &= TARGET_PAGE_MASK;
        tlb_set_page_with_attrs(cs, address, phys_addr, attrs,
                                prot, mmu_idx, page_size);
        return 0;
    }

    return ret;
}

hwaddr arm_cpu_get_phys_page_attrs_debug(CPUState *cs, vaddr addr,
                                         MemTxAttrs *attrs)
{
    ARMCPU *cpu = ARM_CPU(NULL, cs);
    CPUARMState *env = &cpu->env;
    hwaddr phys_addr;
    target_ulong page_size;
    int prot;
    bool ret;
    ARMMMUFaultInfo fi = {0};
    ARMMMUIdx mmu_idx = core_to_arm_mmu_idx(env, cpu_mmu_index(env, false));

    ret = get_phys_addr(env, addr, 0, mmu_idx, &phys_addr,
                        attrs, &prot, &page_size, &fi, NULL);

    if (ret) {
        return -1;
    }
    return phys_addr;
}

uint32_t HELPER(v7m_mrs)(CPUARMState *env, uint32_t reg)
{
    uint32_t mask;
    unsigned el = arm_current_el(env);

    /* First handle registers which unprivileged can read */

    switch (reg) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7: /* xPSR sub-fields */
        mask = 0;
        if ((reg & 1) && el) {
            mask |= XPSR_EXCP; /* IPSR (unpriv. reads as zero) */
        }
        if (!(reg & 4)) {
            mask |= XPSR_NZCV | XPSR_Q; /* APSR */
        }
        /* EPSR reads as zero */
        return xpsr_read(env) & mask;
        break;
    case 20: /* CONTROL */
        return env->v7m.control[env->v7m.secure];
    case 0x94: /* CONTROL_NS */
        /* We have to handle this here because unprivileged Secure code
         * can read the NS CONTROL register.
         */
        if (!env->v7m.secure) {
            return 0;
        }
        return env->v7m.control[M_REG_NS];
    }

    if (el == 0) {
        return 0; /* unprivileged reads others as zero */
    }

    if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
        switch (reg) {
        case 0x88: /* MSP_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.other_ss_msp;
        case 0x89: /* PSP_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.other_ss_psp;
        case 0x8a: /* MSPLIM_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.msplim[M_REG_NS];
        case 0x8b: /* PSPLIM_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.psplim[M_REG_NS];
        case 0x90: /* PRIMASK_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.primask[M_REG_NS];
        case 0x91: /* BASEPRI_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.basepri[M_REG_NS];
        case 0x93: /* FAULTMASK_NS */
            if (!env->v7m.secure) {
                return 0;
            }
            return env->v7m.faultmask[M_REG_NS];
        case 0x98: /* SP_NS */
        {
            /* This gives the non-secure SP selected based on whether we're
             * currently in handler mode or not, using the NS CONTROL.SPSEL.
             */
            bool spsel = env->v7m.control[M_REG_NS] & R_V7M_CONTROL_SPSEL_MASK;

            if (!env->v7m.secure) {
                return 0;
            }
            if (!arm_v7m_is_handler_mode(env) && spsel) {
                return env->v7m.other_ss_psp;
            } else {
                return env->v7m.other_ss_msp;
            }
        }
        default:
            break;
        }
    }

    switch (reg) {
    case 8: /* MSP */
        return v7m_using_psp(env) ? env->v7m.other_sp : env->regs[13];
    case 9: /* PSP */
        return v7m_using_psp(env) ? env->regs[13] : env->v7m.other_sp;
    case 10: /* MSPLIM */
        if (!arm_feature(env, ARM_FEATURE_V8)) {
            goto bad_reg;
        }
        return env->v7m.msplim[env->v7m.secure];
    case 11: /* PSPLIM */
        if (!arm_feature(env, ARM_FEATURE_V8)) {
            goto bad_reg;
        }
        return env->v7m.psplim[env->v7m.secure];
    case 16: /* PRIMASK */
        return env->v7m.primask[env->v7m.secure];
    case 17: /* BASEPRI */
    case 18: /* BASEPRI_MAX */
        return env->v7m.basepri[env->v7m.secure];
    case 19: /* FAULTMASK */
        return env->v7m.faultmask[env->v7m.secure];
    default:
    bad_reg:
        qemu_log_mask(LOG_GUEST_ERROR, "Attempt to read unknown special"
                                       " register %d\n", reg);
        return 0;
    }
}

void HELPER(v7m_msr)(CPUARMState *env, uint32_t maskreg, uint32_t val)
{
    /* We're passed bits [11..0] of the instruction; extract
     * SYSm and the mask bits.
     * Invalid combinations of SYSm and mask are UNPREDICTABLE;
     * we choose to treat them as if the mask bits were valid.
     * NB that the pseudocode 'mask' variable is bits [11..10],
     * whereas ours is [11..8].
     */
    uint32_t mask = extract32(maskreg, 8, 4);
    uint32_t reg = extract32(maskreg, 0, 8);

    if (arm_current_el(env) == 0 && reg > 7) {
        /* only xPSR sub-fields may be written by unprivileged */
        return;
    }

    if (arm_feature(env, ARM_FEATURE_M_SECURITY)) {
        switch (reg) {
        case 0x88: /* MSP_NS */
            if (!env->v7m.secure) {
                return;
            }
            env->v7m.other_ss_msp = val;
            return;
        case 0x89: /* PSP_NS */
            if (!env->v7m.secure) {
                return;
            }
            env->v7m.other_ss_psp = val;
            return;
        case 0x8a: /* MSPLIM_NS */
            if (!env->v7m.secure) {
                return;
            }
            env->v7m.msplim[M_REG_NS] = val & ~7;
            return;
        case 0x8b: /* PSPLIM_NS */
            if (!env->v7m.secure) {
                return;
            }
            env->v7m.psplim[M_REG_NS] = val & ~7;
            return;
        case 0x90: /* PRIMASK_NS */
            if (!env->v7m.secure) {
                return;
            }
            env->v7m.primask[M_REG_NS] = val & 1;
            return;
        case 0x91: /* BASEPRI_NS */
            if (!env->v7m.secure) {
                return;
            }
            env->v7m.basepri[M_REG_NS] = val & 0xff;
            return;
        case 0x93: /* FAULTMASK_NS */
            if (!env->v7m.secure) {
                return;
            }
            env->v7m.faultmask[M_REG_NS] = val & 1;
            return;
        case 0x94: /* CONTROL_NS */
            if (!env->v7m.secure) {
                return;
            }
            write_v7m_control_spsel_for_secstate(env,
                                                 val & R_V7M_CONTROL_SPSEL_MASK,
                                                 M_REG_NS);
            env->v7m.control[M_REG_NS] &= ~R_V7M_CONTROL_NPRIV_MASK;
            env->v7m.control[M_REG_NS] |= val & R_V7M_CONTROL_NPRIV_MASK;
            return;
        case 0x98: /* SP_NS */
        {
            /* This gives the non-secure SP selected based on whether we're
             * currently in handler mode or not, using the NS CONTROL.SPSEL.
             */
            bool spsel = env->v7m.control[M_REG_NS] & R_V7M_CONTROL_SPSEL_MASK;

            if (!env->v7m.secure) {
                return;
            }
            if (!arm_v7m_is_handler_mode(env) && spsel) {
                env->v7m.other_ss_psp = val;
            } else {
                env->v7m.other_ss_msp = val;
            }
            return;
        }
        default:
            break;
        }
    }

    switch (reg) {
    case 0:
    case 1:
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7: /* xPSR sub-fields */
        /* only APSR is actually writable */
        if (!(reg & 4)) {
            uint32_t apsrmask = 0;

            if (mask & 8) {
                apsrmask |= XPSR_NZCV | XPSR_Q;
            }
            if ((mask & 4) && arm_feature(env, ARM_FEATURE_THUMB_DSP)) {
                apsrmask |= XPSR_GE;
            }
            xpsr_write(env, val, apsrmask);
        }
        break;
    case 8: /* MSP */
        if (v7m_using_psp(env)) {
            env->v7m.other_sp = val;
        } else {
            env->regs[13] = val;
        }
        break;
    case 9: /* PSP */
        if (v7m_using_psp(env)) {
            env->regs[13] = val;
        } else {
            env->v7m.other_sp = val;
        }
        break;
    case 10: /* MSPLIM */
        if (!arm_feature(env, ARM_FEATURE_V8)) {
            goto bad_reg;
        }
        env->v7m.msplim[env->v7m.secure] = val & ~7;
        break;
    case 11: /* PSPLIM */
        if (!arm_feature(env, ARM_FEATURE_V8)) {
            goto bad_reg;
        }
        env->v7m.psplim[env->v7m.secure] = val & ~7;
        break;
    case 16: /* PRIMASK */
        env->v7m.primask[env->v7m.secure] = val & 1;
        break;
    case 17: /* BASEPRI */
        env->v7m.basepri[env->v7m.secure] = val & 0xff;
        break;
    case 18: /* BASEPRI_MAX */
        val &= 0xff;
        if (val != 0 && (val < env->v7m.basepri[env->v7m.secure]
                         || env->v7m.basepri[env->v7m.secure] == 0)) {
            env->v7m.basepri[env->v7m.secure] = val;
        }
        break;
    case 19: /* FAULTMASK */
        env->v7m.faultmask[env->v7m.secure] = val & 1;
        break;
    case 20: /* CONTROL */
        /* Writing to the SPSEL bit only has an effect if we are in
         * thread mode; other bits can be updated by any privileged code.
         * write_v7m_control_spsel() deals with updating the SPSEL bit in
         * env->v7m.control, so we only need update the others.
         * For v7M, we must just ignore explicit writes to SPSEL in handler
         * mode; for v8M the write is permitted but will have no effect.
         */
        if (arm_feature(env, ARM_FEATURE_V8) ||
            !arm_v7m_is_handler_mode(env)) {
            write_v7m_control_spsel(env, (val & R_V7M_CONTROL_SPSEL_MASK) != 0);
        }
        env->v7m.control[env->v7m.secure] &= ~R_V7M_CONTROL_NPRIV_MASK;
        env->v7m.control[env->v7m.secure] |= val & R_V7M_CONTROL_NPRIV_MASK;
        break;
    default:
    bad_reg:
        qemu_log_mask(LOG_GUEST_ERROR, "Attempt to write unknown special"
                                       " register %d\n", reg);
        return;
    }
}

uint32_t HELPER(v7m_tt)(CPUARMState *env, uint32_t addr, uint32_t op)
{
    /* Implement the TT instruction. op is bits [7:6] of the insn. */
    bool forceunpriv = op & 1;
    bool alt = op & 2;
    V8M_SAttributes sattrs = {0};
    uint32_t tt_resp;
    bool r, rw, nsr, nsrw, mrvalid;
    int prot;
    ARMMMUFaultInfo fi = {0};
    MemTxAttrs attrs = {0};
    hwaddr phys_addr;
    ARMMMUIdx mmu_idx;
    uint32_t mregion;
    bool targetpriv;
    bool targetsec = env->v7m.secure;

    /* Work out what the security state and privilege level we're
     * interested in is...
     */
    if (alt) {
        targetsec = !targetsec;
    }

    if (forceunpriv) {
        targetpriv = false;
    } else {
        targetpriv = arm_v7m_is_handler_mode(env) ||
            !(env->v7m.control[targetsec] & R_V7M_CONTROL_NPRIV_MASK);
    }

    /* ...and then figure out which MMU index this is */
    mmu_idx = arm_v7m_mmu_idx_for_secstate_and_priv(env, targetsec, targetpriv);

    /* We know that the MPU and SAU don't care about the access type
     * for our purposes beyond that we don't want to claim to be
     * an insn fetch, so we arbitrarily call this a read.
     */

    /* MPU region info only available for privileged or if
     * inspecting the other MPU state.
     */
    if (arm_current_el(env) != 0 || alt) {
        /* We can ignore the return value as prot is always set */
        pmsav8_mpu_lookup(env, addr, MMU_DATA_LOAD, mmu_idx,
                          &phys_addr, &attrs, &prot, &fi, &mregion);
        if (mregion == -1) {
            mrvalid = false;
            mregion = 0;
        } else {
            mrvalid = true;
        }
        r = prot & PAGE_READ;
        rw = prot & PAGE_WRITE;
    } else {
        r = false;
        rw = false;
        mrvalid = false;
        mregion = 0;
    }

    if (env->v7m.secure) {
        v8m_security_lookup(env, addr, MMU_DATA_LOAD, mmu_idx, &sattrs);
        nsr = sattrs.ns && r;
        nsrw = sattrs.ns && rw;
    } else {
        sattrs.ns = true;
        nsr = false;
        nsrw = false;
    }

    tt_resp = (sattrs.iregion << 24) |
        (sattrs.irvalid << 23) |
        ((!sattrs.ns) << 22) |
        (nsrw << 21) |
        (nsr << 20) |
        (rw << 19) |
        (r << 18) |
        (sattrs.srvalid << 17) |
        (mrvalid << 16) |
        (sattrs.sregion << 8) |
        mregion;

    return tt_resp;
}

#endif

void HELPER(dc_zva)(CPUARMState *env, uint64_t vaddr_in)
{
    /* Implement DC ZVA, which zeroes a fixed-length block of memory.
     * Note that we do not implement the (architecturally mandated)
     * alignment fault for attempts to use this on Device memory
     * (which matches the usual QEMU behaviour of not implementing either
     * alignment faults or any memory attribute handling).
     */

    ARMCPU *cpu = arm_env_get_cpu(env);
    uint64_t blocklen = 4 << cpu->dcz_blocksize;
    uint64_t vaddr = vaddr_in & ~(blocklen - 1);

#ifndef CONFIG_USER_ONLY
    {
        /* Slightly awkwardly, QEMU's TARGET_PAGE_SIZE may be less than
         * the block size so we might have to do more than one TLB lookup.
         * We know that in fact for any v8 CPU the page size is at least 4K
         * and the block size must be 2K or less, but TARGET_PAGE_SIZE is only
         * 1K as an artefact of legacy v5 subpage support being present in the
         * same QEMU executable.
         */
        
        int maxidx = DIV_ROUND_UP(blocklen, TARGET_PAGE_SIZE);
        // msvc doesnt allow non-constant array sizes, so we work out the size it would be
        // TARGET_PAGE_SIZE is 1024
        // blocklen is 64
        // maxidx = (blocklen+TARGET_PAGE_SIZE-1) / TARGET_PAGE_SIZE
        //        = (64+1024-1) / 1024
        //        = 1
#ifdef _MSC_VER
        void *hostaddr[1];
#else
        void *hostaddr[maxidx];
#endif
        int try, i;
        unsigned mmu_idx = cpu_mmu_index(env, false);
        TCGMemOpIdx oi = make_memop_idx(MO_UB, mmu_idx);

        for (try = 0; try < 2; try++) {

            for (i = 0; i < maxidx; i++) {
                hostaddr[i] = tlb_vaddr_to_host(env,
                                                vaddr + TARGET_PAGE_SIZE * i,
                                                1, mmu_idx);
                if (!hostaddr[i]) {
                    break;
                }
            }
            if (i == maxidx) {
                /* If it's all in the TLB it's fair game for just writing to;
                 * we know we don't need to update dirty status, etc.
                 */
                for (i = 0; i < maxidx - 1; i++) {
                    memset(hostaddr[i], 0, TARGET_PAGE_SIZE);
                }
                memset(hostaddr[i], 0, blocklen - (i * TARGET_PAGE_SIZE));
                return;
            }
            /* OK, try a store and see if we can populate the tlb. This
             * might cause an exception if the memory isn't writable,
             * in which case we will longjmp out of here. We must for
             * this purpose use the actual register value passed to us
             * so that we get the fault address right.
             */
            helper_ret_stb_mmu(env, vaddr_in, 0, oi, GETPC());
            /* Now we can populate the other TLB entries, if any */
            for (i = 0; i < maxidx; i++) {
                uint64_t va = vaddr + TARGET_PAGE_SIZE * i;
                if (va != (vaddr_in & TARGET_PAGE_MASK)) {
                    helper_ret_stb_mmu(env, va, 0, oi, GETPC());
                }
            }
        }

        /* Slow path (probably attempt to do this to an I/O device or
         * similar, or clearing of a block of code we have translations
         * cached for). Just do a series of byte writes as the architecture
         * demands. It's not worth trying to use a cpu_physical_memory_map(),
         * memset(), unmap() sequence here because:
         *  + we'd need to account for the blocksize being larger than a page
         *  + the direct-RAM access case is almost always going to be dealt
         *    with in the fastpath code above, so there's no speed benefit
         *  + we would have to deal with the map returning NULL because the
         *    bounce buffer was in use
         */
        for (i = 0; i < blocklen; i++) {
            helper_ret_stb_mmu(env, vaddr + i, 0, oi, GETPC());
        }
    }
#else
    memset(g2h(vaddr), 0, blocklen);
#endif
}

/* Note that signed overflow is undefined in C.  The following routines are
   careful to use unsigned types where modulo arithmetic is required.
   Failure to do so _will_ break on newer gcc.  */

/* Signed saturating arithmetic.  */

/* Perform 16-bit signed saturating addition.  */
static inline uint16_t add16_sat(uint16_t a, uint16_t b)
{
    uint16_t res;

    res = a + b;
    if (((res ^ a) & 0x8000) && !((a ^ b) & 0x8000)) {
        if (a & 0x8000)
            res = 0x8000;
        else
            res = 0x7fff;
    }
    return res;
}

/* Perform 8-bit signed saturating addition.  */
static inline uint8_t add8_sat(uint8_t a, uint8_t b)
{
    uint8_t res;

    res = a + b;
    if (((res ^ a) & 0x80) && !((a ^ b) & 0x80)) {
        if (a & 0x80)
            res = 0x80;
        else
            res = 0x7f;
    }
    return res;
}

/* Perform 16-bit signed saturating subtraction.  */
static inline uint16_t sub16_sat(uint16_t a, uint16_t b)
{
    uint16_t res;

    res = a - b;
    if (((res ^ a) & 0x8000) && ((a ^ b) & 0x8000)) {
        if (a & 0x8000)
            res = 0x8000;
        else
            res = 0x7fff;
    }
    return res;
}

/* Perform 8-bit signed saturating subtraction.  */
static inline uint8_t sub8_sat(uint8_t a, uint8_t b)
{
    uint8_t res;

    res = a - b;
    if (((res ^ a) & 0x80) && ((a ^ b) & 0x80)) {
        if (a & 0x80)
            res = 0x80;
        else
            res = 0x7f;
    }
    return res;
}

#define ADD16(a, b, n) RESULT(add16_sat(a, b), n, 16);
#define SUB16(a, b, n) RESULT(sub16_sat(a, b), n, 16);
#define ADD8(a, b, n)  RESULT(add8_sat(a, b), n, 8);
#define SUB8(a, b, n)  RESULT(sub8_sat(a, b), n, 8);
#define PFX q

#include "op_addsub.h"

/* Unsigned saturating arithmetic.  */
static inline uint16_t add16_usat(uint16_t a, uint16_t b)
{
    uint16_t res;
    res = a + b;
    if (res < a)
        res = 0xffff;
    return res;
}

static inline uint16_t sub16_usat(uint16_t a, uint16_t b)
{
    if (a > b)
        return a - b;
    else
        return 0;
}

static inline uint8_t add8_usat(uint8_t a, uint8_t b)
{
    uint8_t res;
    res = a + b;
    if (res < a)
        res = 0xff;
    return res;
}

static inline uint8_t sub8_usat(uint8_t a, uint8_t b)
{
    if (a > b)
        return a - b;
    else
        return 0;
}

#define ADD16(a, b, n) RESULT(add16_usat(a, b), n, 16);
#define SUB16(a, b, n) RESULT(sub16_usat(a, b), n, 16);
#define ADD8(a, b, n)  RESULT(add8_usat(a, b), n, 8);
#define SUB8(a, b, n)  RESULT(sub8_usat(a, b), n, 8);
#define PFX uq

#include "op_addsub.h"

/* Signed modulo arithmetic.  */
#define SARITH16(a, b, n, op) do { \
    int32_t sum; \
    sum = (int32_t)(int16_t)(a) op (int32_t)(int16_t)(b); \
    RESULT(sum, n, 16); \
    if (sum >= 0) \
        ge |= 3 << (n * 2); \
    } while(0)

#define SARITH8(a, b, n, op) do { \
    int32_t sum; \
    sum = (int32_t)(int8_t)(a) op (int32_t)(int8_t)(b); \
    RESULT(sum, n, 8); \
    if (sum >= 0) \
        ge |= 1 << n; \
    } while(0)


#define ADD16(a, b, n) SARITH16(a, b, n, +)
#define SUB16(a, b, n) SARITH16(a, b, n, -)
#define ADD8(a, b, n)  SARITH8(a, b, n, +)
#define SUB8(a, b, n)  SARITH8(a, b, n, -)
#define PFX s
#define ARITH_GE

#include "op_addsub.h"

/* Unsigned modulo arithmetic.  */
#define ADD16(a, b, n) do { \
    uint32_t sum; \
    sum = (uint32_t)(uint16_t)(a) + (uint32_t)(uint16_t)(b); \
    RESULT(sum, n, 16); \
    if ((sum >> 16) == 1) \
        ge |= 3 << (n * 2); \
    } while(0)

#define ADD8(a, b, n) do { \
    uint32_t sum; \
    sum = (uint32_t)(uint8_t)(a) + (uint32_t)(uint8_t)(b); \
    RESULT(sum, n, 8); \
    if ((sum >> 8) == 1) \
        ge |= 1 << n; \
    } while(0)

#define SUB16(a, b, n) do { \
    uint32_t sum; \
    sum = (uint32_t)(uint16_t)(a) - (uint32_t)(uint16_t)(b); \
    RESULT(sum, n, 16); \
    if ((sum >> 16) == 0) \
        ge |= 3 << (n * 2); \
    } while(0)

#define SUB8(a, b, n) do { \
    uint32_t sum; \
    sum = (uint32_t)(uint8_t)(a) - (uint32_t)(uint8_t)(b); \
    RESULT(sum, n, 8); \
    if ((sum >> 8) == 0) \
        ge |= 1 << n; \
    } while(0)

#define PFX u
#define ARITH_GE

#include "op_addsub.h"

/* Halved signed arithmetic.  */
#define ADD16(a, b, n) \
  RESULT(((int32_t)(int16_t)(a) + (int32_t)(int16_t)(b)) >> 1, n, 16)
#define SUB16(a, b, n) \
  RESULT(((int32_t)(int16_t)(a) - (int32_t)(int16_t)(b)) >> 1, n, 16)
#define ADD8(a, b, n) \
  RESULT(((int32_t)(int8_t)(a) + (int32_t)(int8_t)(b)) >> 1, n, 8)
#define SUB8(a, b, n) \
  RESULT(((int32_t)(int8_t)(a) - (int32_t)(int8_t)(b)) >> 1, n, 8)
#define PFX sh

#include "op_addsub.h"

/* Halved unsigned arithmetic.  */
#define ADD16(a, b, n) \
  RESULT(((uint32_t)(uint16_t)(a) + (uint32_t)(uint16_t)(b)) >> 1, n, 16)
#define SUB16(a, b, n) \
  RESULT(((uint32_t)(uint16_t)(a) - (uint32_t)(uint16_t)(b)) >> 1, n, 16)
#define ADD8(a, b, n) \
  RESULT(((uint32_t)(uint8_t)(a) + (uint32_t)(uint8_t)(b)) >> 1, n, 8)
#define SUB8(a, b, n) \
  RESULT(((uint32_t)(uint8_t)(a) - (uint32_t)(uint8_t)(b)) >> 1, n, 8)
#define PFX uh

#include "op_addsub.h"

static inline uint8_t do_usad(uint8_t a, uint8_t b)
{
    if (a > b)
        return a - b;
    else
        return b - a;
}

/* Unsigned sum of absolute byte differences.  */
uint32_t HELPER(usad8)(uint32_t a, uint32_t b)
{
    uint32_t sum;
    sum = do_usad(a, b);
    sum += do_usad(a >> 8, b >> 8);
    sum += do_usad(a >> 16, b >>16);
    sum += do_usad(a >> 24, b >> 24);
    return sum;
}

/* For ARMv6 SEL instruction.  */
uint32_t HELPER(sel_flags)(uint32_t flags, uint32_t a, uint32_t b)
{
    uint32_t mask;

    mask = 0;
    if (flags & 1)
        mask |= 0xff;
    if (flags & 2)
        mask |= 0xff00;
    if (flags & 4)
        mask |= 0xff0000;
    if (flags & 8)
        mask |= 0xff000000;
    return (a & mask) | (b & ~mask);
}

/* VFP support.  We follow the convention used for VFP instructions:
   Single precision routines have a "s" suffix, double precision a
   "d" suffix.  */

/* Convert host exception flags to vfp form.  */
static inline int vfp_exceptbits_from_host(int host_bits)
{
    int target_bits = 0;

    if (host_bits & float_flag_invalid)
        target_bits |= 1;
    if (host_bits & float_flag_divbyzero)
        target_bits |= 2;
    if (host_bits & float_flag_overflow)
        target_bits |= 4;
    if (host_bits & (float_flag_underflow | float_flag_output_denormal))
        target_bits |= 8;
    if (host_bits & float_flag_inexact)
        target_bits |= 0x10;
    if (host_bits & float_flag_input_denormal)
        target_bits |= 0x80;
    return target_bits;
}

uint32_t HELPER(vfp_get_fpscr)(CPUARMState *env)
{
    int i;
    uint32_t fpscr;

    fpscr = (env->vfp.xregs[ARM_VFP_FPSCR] & 0xffc8ffff)
            | (env->vfp.vec_len << 16)
            | (env->vfp.vec_stride << 20);
    i = get_float_exception_flags(&env->vfp.fp_status);
    i |= get_float_exception_flags(&env->vfp.standard_fp_status);
    i |= get_float_exception_flags(&env->vfp.fp_status_f16);
    fpscr |= vfp_exceptbits_from_host(i);
    return fpscr;
}

uint32_t vfp_get_fpscr(CPUARMState *env)
{
    return HELPER(vfp_get_fpscr)(env);
}

/* Convert vfp exception flags to target form.  */
static inline int vfp_exceptbits_to_host(int target_bits)
{
    int host_bits = 0;

    if (target_bits & 1)
        host_bits |= float_flag_invalid;
    if (target_bits & 2)
        host_bits |= float_flag_divbyzero;
    if (target_bits & 4)
        host_bits |= float_flag_overflow;
    if (target_bits & 8)
        host_bits |= float_flag_underflow;
    if (target_bits & 0x10)
        host_bits |= float_flag_inexact;
    if (target_bits & 0x80)
        host_bits |= float_flag_input_denormal;
    return host_bits;
}

void HELPER(vfp_set_fpscr)(CPUARMState *env, uint32_t val)
{
    int i;
    uint32_t changed;

    changed = env->vfp.xregs[ARM_VFP_FPSCR];
    env->vfp.xregs[ARM_VFP_FPSCR] = (val & 0xffc8ffff);
    env->vfp.vec_len = (val >> 16) & 7;
    env->vfp.vec_stride = (val >> 20) & 3;

    changed ^= val;
    if (changed & (3 << 22)) {
        i = (val >> 22) & 3;
        switch (i) {
        case FPROUNDING_TIEEVEN:
            i = float_round_nearest_even;
            break;
        case FPROUNDING_POSINF:
            i = float_round_up;
            break;
        case FPROUNDING_NEGINF:
            i = float_round_down;
            break;
        case FPROUNDING_ZERO:
            i = float_round_to_zero;
            break;
        }
        set_float_rounding_mode(i, &env->vfp.fp_status);
        set_float_rounding_mode(i, &env->vfp.fp_status_f16);
    }
    if (changed & FPCR_FZ16) {
        bool ftz_enabled = val & FPCR_FZ16;
        set_flush_to_zero(ftz_enabled, &env->vfp.fp_status_f16);
        set_flush_inputs_to_zero(ftz_enabled, &env->vfp.fp_status_f16);
    }
    if (changed & FPCR_FZ) {
        bool ftz_enabled = val & FPCR_FZ;
        set_flush_to_zero(ftz_enabled, &env->vfp.fp_status);
        set_flush_inputs_to_zero(ftz_enabled, &env->vfp.fp_status);
    }
    if (changed & FPCR_DN) {
        bool dnan_enabled = val & FPCR_DN;
        set_default_nan_mode(dnan_enabled, &env->vfp.fp_status);
        set_default_nan_mode(dnan_enabled, &env->vfp.fp_status_f16);
    }

    /* The exception flags are ORed together when we read fpscr so we
     * only need to preserve the current state in one of our
     * float_status values.
     */
    i = vfp_exceptbits_to_host(val);
    set_float_exception_flags(i, &env->vfp.fp_status);
    set_float_exception_flags(0, &env->vfp.fp_status_f16);
    set_float_exception_flags(0, &env->vfp.standard_fp_status);
}

void vfp_set_fpscr(CPUARMState *env, uint32_t val)
{
    HELPER(vfp_set_fpscr)(env, val);
}

#define VFP_HELPER(name, p) HELPER(glue(glue(vfp_,name),p))

#define VFP_BINOP(name) \
float32 VFP_HELPER(name, s)(float32 a, float32 b, void *fpstp) \
{ \
    float_status *fpst = fpstp; \
    return float32_ ## name(a, b, fpst); \
} \
float64 VFP_HELPER(name, d)(float64 a, float64 b, void *fpstp) \
{ \
    float_status *fpst = fpstp; \
    return float64_ ## name(a, b, fpst); \
}
VFP_BINOP(add)
VFP_BINOP(sub)
VFP_BINOP(mul)
VFP_BINOP(div)
VFP_BINOP(min)
VFP_BINOP(max)
VFP_BINOP(minnum)
VFP_BINOP(maxnum)
#undef VFP_BINOP

float32 VFP_HELPER(neg, s)(float32 a)
{
    return float32_chs(a);
}

float64 VFP_HELPER(neg, d)(float64 a)
{
    return float64_chs(a);
}

float32 VFP_HELPER(abs, s)(float32 a)
{
    return float32_abs(a);
}

float64 VFP_HELPER(abs, d)(float64 a)
{
    return float64_abs(a);
}

float32 VFP_HELPER(sqrt, s)(float32 a, CPUARMState *env)
{
    return float32_sqrt(a, &env->vfp.fp_status);
}

float64 VFP_HELPER(sqrt, d)(float64 a, CPUARMState *env)
{
    return float64_sqrt(a, &env->vfp.fp_status);
}

/* XXX: check quiet/signaling case */
#define DO_VFP_cmp(p, type) \
void VFP_HELPER(cmp, p)(type a, type b, CPUARMState *env)  \
{ \
    uint32_t flags; \
    switch(type ## _compare_quiet(a, b, &env->vfp.fp_status)) { \
    case 0: flags = 0x6; break; \
    case -1: flags = 0x8; break; \
    case 1: flags = 0x2; break; \
    default: case 2: flags = 0x3; break; \
    } \
    env->vfp.xregs[ARM_VFP_FPSCR] = (flags << 28) \
        | (env->vfp.xregs[ARM_VFP_FPSCR] & 0x0fffffff); \
} \
void VFP_HELPER(cmpe, p)(type a, type b, CPUARMState *env) \
{ \
    uint32_t flags; \
    switch(type ## _compare(a, b, &env->vfp.fp_status)) { \
    case 0: flags = 0x6; break; \
    case -1: flags = 0x8; break; \
    case 1: flags = 0x2; break; \
    default: case 2: flags = 0x3; break; \
    } \
    env->vfp.xregs[ARM_VFP_FPSCR] = (flags << 28) \
        | (env->vfp.xregs[ARM_VFP_FPSCR] & 0x0fffffff); \
}
DO_VFP_cmp(s, float32)
DO_VFP_cmp(d, float64)
#undef DO_VFP_cmp

/* Integer to float and float to integer conversions */

#define CONV_ITOF(name, fsz, sign) \
    float##fsz HELPER(name)(uint32_t x, void *fpstp) \
{ \
    float_status *fpst = fpstp; \
    return sign##int32_to_##float##fsz((sign##int32_t)x, fpst); \
}

#define CONV_FTOI(name, fsz, sign, round) \
uint32_t HELPER(name)(float##fsz x, void *fpstp) \
{ \
    float_status *fpst = fpstp; \
    if (float##fsz##_is_any_nan(x)) { \
        float_raise(float_flag_invalid, fpst); \
        return 0; \
    } \
    return float##fsz##_to_##sign##int32##round(x, fpst); \
}

#define FLOAT_CONVS(name, p, fsz, sign) \
CONV_ITOF(vfp_##name##to##p, fsz, sign) \
CONV_FTOI(vfp_to##name##p, fsz, sign, ) \
CONV_FTOI(vfp_to##name##z##p, fsz, sign, _round_to_zero)

FLOAT_CONVS(si, h, 16, )
FLOAT_CONVS(si, s, 32, )
FLOAT_CONVS(si, d, 64, )
FLOAT_CONVS(ui, h, 16, u)
FLOAT_CONVS(ui, s, 32, u)
FLOAT_CONVS(ui, d, 64, u)

#undef CONV_ITOF
#undef CONV_FTOI
#undef FLOAT_CONVS

/* floating point conversion */
float64 VFP_HELPER(fcvtd, s)(float32 x, CPUARMState *env)
{
    float64 r = float32_to_float64(x, &env->vfp.fp_status);
    /* ARM requires that S<->D conversion of any kind of NaN generates
     * a quiet NaN by forcing the most significant frac bit to 1.
     */
    return float64_maybe_silence_nan(r, &env->vfp.fp_status);
}

float32 VFP_HELPER(fcvts, d)(float64 x, CPUARMState *env)
{
    float32 r =  float64_to_float32(x, &env->vfp.fp_status);
    /* ARM requires that S<->D conversion of any kind of NaN generates
     * a quiet NaN by forcing the most significant frac bit to 1.
     */
    return float32_maybe_silence_nan(r, &env->vfp.fp_status);
}

/* VFP3 fixed point conversion.  */
#define VFP_CONV_FIX_FLOAT(name, p, fsz, isz, itype) \
float##fsz HELPER(vfp_##name##to##p)(uint##isz##_t  x, uint32_t shift, \
                                     void *fpstp) \
{ \
    float_status *fpst = fpstp; \
    float##fsz tmp; \
    tmp = itype##_to_##float##fsz(x, fpst); \
    return float##fsz##_scalbn(tmp, -(int)shift, fpst); \
}

/* Notice that we want only input-denormal exception flags from the
 * scalbn operation: the other possible flags (overflow+inexact if
 * we overflow to infinity, output-denormal) aren't correct for the
 * complete scale-and-convert operation.
 */
#define VFP_CONV_FLOAT_FIX_ROUND(name, p, fsz, isz, itype, round) \
uint##isz##_t HELPER(vfp_to##name##p##round)(float##fsz x, \
                                             uint32_t shift, \
                                             void *fpstp) \
{ \
    float_status *fpst = fpstp; \
    int old_exc_flags = get_float_exception_flags(fpst); \
    float##fsz tmp; \
    if (float##fsz##_is_any_nan(x)) { \
        float_raise(float_flag_invalid, fpst); \
        return 0; \
    } \
    tmp = float##fsz##_scalbn(x, shift, fpst); \
    old_exc_flags |= get_float_exception_flags(fpst) \
        & float_flag_input_denormal; \
    set_float_exception_flags(old_exc_flags, fpst); \
    return float##fsz##_to_##itype##round(tmp, fpst); \
}

#define VFP_CONV_FIX(name, p, fsz, isz, itype)                   \
VFP_CONV_FIX_FLOAT(name, p, fsz, isz, itype)                     \
VFP_CONV_FLOAT_FIX_ROUND(name, p, fsz, isz, itype, _round_to_zero) \
VFP_CONV_FLOAT_FIX_ROUND(name, p, fsz, isz, itype, )

#define VFP_CONV_FIX_A64(name, p, fsz, isz, itype)               \
VFP_CONV_FIX_FLOAT(name, p, fsz, isz, itype)                     \
VFP_CONV_FLOAT_FIX_ROUND(name, p, fsz, isz, itype, )

VFP_CONV_FIX(sh, d, 64, 64, int16)
VFP_CONV_FIX(sl, d, 64, 64, int32)
VFP_CONV_FIX_A64(sq, d, 64, 64, int64)
VFP_CONV_FIX(uh, d, 64, 64, uint16)
VFP_CONV_FIX(ul, d, 64, 64, uint32)
VFP_CONV_FIX_A64(uq, d, 64, 64, uint64)
VFP_CONV_FIX(sh, s, 32, 32, int16)
VFP_CONV_FIX(sl, s, 32, 32, int32)
VFP_CONV_FIX_A64(sq, s, 32, 64, int64)
VFP_CONV_FIX(uh, s, 32, 32, uint16)
VFP_CONV_FIX(ul, s, 32, 32, uint32)
VFP_CONV_FIX_A64(uq, s, 32, 64, uint64)
VFP_CONV_FIX_A64(sl, h, 16, 32, int32)
VFP_CONV_FIX_A64(ul, h, 16, 32, uint32)
#undef VFP_CONV_FIX
#undef VFP_CONV_FIX_FLOAT
#undef VFP_CONV_FLOAT_FIX_ROUND

/* Set the current fp rounding mode and return the old one.
 * The argument is a softfloat float_round_ value.
 */
uint32_t HELPER(set_rmode)(uint32_t rmode, void *fpstp)
{
    float_status *fp_status = fpstp;

    uint32_t prev_rmode = get_float_rounding_mode(fp_status);
    set_float_rounding_mode(rmode, fp_status);

    return prev_rmode;
}

/* Set the current fp rounding mode in the standard fp status and return
 * the old one. This is for NEON instructions that need to change the
 * rounding mode but wish to use the standard FPSCR values for everything
 * else. Always set the rounding mode back to the correct value after
 * modifying it.
 * The argument is a softfloat float_round_ value.
 */
uint32_t HELPER(set_neon_rmode)(uint32_t rmode, CPUARMState *env)
{
    float_status *fp_status = &env->vfp.standard_fp_status;

    uint32_t prev_rmode = get_float_rounding_mode(fp_status);
    set_float_rounding_mode(rmode, fp_status);

    return prev_rmode;
}

/* Half precision conversions.  */
static float32 do_fcvt_f16_to_f32(uint32_t a, CPUARMState *env, float_status *s)
{
    int ieee = (env->vfp.xregs[ARM_VFP_FPSCR] & (1 << 26)) == 0;
    float32 r = float16_to_float32(make_float16(a), ieee, s);
    if (ieee) {
        return float32_maybe_silence_nan(r, s);
    }
    return r;
}

static uint32_t do_fcvt_f32_to_f16(float32 a, CPUARMState *env, float_status *s)
{
    int ieee = (env->vfp.xregs[ARM_VFP_FPSCR] & (1 << 26)) == 0;
    float16 r = float32_to_float16(a, ieee, s);
    if (ieee) {
        r = float16_maybe_silence_nan(r, s);
    }
    return float16_val(r);
}

float32 HELPER(neon_fcvt_f16_to_f32)(uint32_t a, CPUARMState *env)
{
    return do_fcvt_f16_to_f32(a, env, &env->vfp.standard_fp_status);
}

uint32_t HELPER(neon_fcvt_f32_to_f16)(float32 a, CPUARMState *env)
{
    return do_fcvt_f32_to_f16(a, env, &env->vfp.standard_fp_status);
}

float32 HELPER(vfp_fcvt_f16_to_f32)(uint32_t a, CPUARMState *env)
{
    return do_fcvt_f16_to_f32(a, env, &env->vfp.fp_status);
}

uint32_t HELPER(vfp_fcvt_f32_to_f16)(float32 a, CPUARMState *env)
{
    return do_fcvt_f32_to_f16(a, env, &env->vfp.fp_status);
}

float64 HELPER(vfp_fcvt_f16_to_f64)(uint32_t a, CPUARMState *env)
{
    int ieee = (env->vfp.xregs[ARM_VFP_FPSCR] & (1 << 26)) == 0;
    float64 r = float16_to_float64(make_float16(a), ieee, &env->vfp.fp_status);
    if (ieee) {
        return float64_maybe_silence_nan(r, &env->vfp.fp_status);
    }
    return r;
}

uint32_t HELPER(vfp_fcvt_f64_to_f16)(float64 a, CPUARMState *env)
{
    int ieee = (env->vfp.xregs[ARM_VFP_FPSCR] & (1 << 26)) == 0;
    float16 r = float64_to_float16(a, ieee, &env->vfp.fp_status);
    if (ieee) {
        r = float16_maybe_silence_nan(r, &env->vfp.fp_status);
    }
    return float16_val(r);
}

#define float32_two make_float32(0x40000000)
#define float32_three make_float32(0x40400000)
#define float32_one_point_five make_float32(0x3fc00000)

float32 HELPER(recps_f32)(float32 a, float32 b, CPUARMState *env)
{
    float_status *s = &env->vfp.standard_fp_status;
    if ((float32_is_infinity(a) && float32_is_zero_or_denormal(b)) ||
        (float32_is_infinity(b) && float32_is_zero_or_denormal(a))) {
        if (!(float32_is_zero(a) || float32_is_zero(b))) {
            float_raise(float_flag_input_denormal, s);
        }
        return float32_two;
    }
    return float32_sub(float32_two, float32_mul(a, b, s), s);
}

float32 HELPER(rsqrts_f32)(float32 a, float32 b, CPUARMState *env)
{
    float_status *s = &env->vfp.standard_fp_status;
    float32 product;
    if ((float32_is_infinity(a) && float32_is_zero_or_denormal(b)) ||
        (float32_is_infinity(b) && float32_is_zero_or_denormal(a))) {
        if (!(float32_is_zero(a) || float32_is_zero(b))) {
            float_raise(float_flag_input_denormal, s);
        }
        return float32_one_point_five;
    }
    product = float32_mul(a, b, s);
    return float32_div(float32_sub(float32_three, product, s), float32_two, s);
}

/* NEON helpers.  */

/* Constants 256 and 512 are used in some helpers; we avoid relying on
 * int->float conversions at run-time.  */
#define float64_256 make_float64(0x4070000000000000LL)
#define float64_512 make_float64(0x4080000000000000LL)
#define float16_maxnorm make_float16(0x7bff)
#define float32_maxnorm make_float32(0x7f7fffff)
#define float64_maxnorm make_float64(0x7fefffffffffffffLL)

/* Reciprocal functions
 *
 * The algorithm that must be used to calculate the estimate
 * is specified by the ARM ARM, see FPRecipEstimate()/RecipEstimate
 */

/* See RecipEstimate()
 *
 * input is a 9 bit fixed point number
 * input range 256 .. 511 for a number from 0.5 <= x < 1.0.
 * result range 256 .. 511 for a number from 1.0 to 511/256.
 */

static int recip_estimate(int input)
{
    int a, b, r;
    assert(256 <= input && input < 512);
    a = (input * 2) + 1;
    b = (1 << 19) / a;
    r = (b + 1) >> 1;
    assert(256 <= r && r < 512);
    return r;
}

/*
 * Common wrapper to call recip_estimate
 *
 * The parameters are exponent and 64 bit fraction (without implicit
 * bit) where the binary point is nominally at bit 52. Returns a
 * float64 which can then be rounded to the appropriate size by the
 * callee.
 */

static uint64_t call_recip_estimate(int *exp, int exp_off, uint64_t frac)
{
    uint32_t scaled, estimate;
    uint64_t result_frac;
    int result_exp;

    /* Handle sub-normals */
    if (*exp == 0) {
        if (extract64(frac, 51, 1) == 0) {
            *exp = -1;
            frac <<= 2;
        } else {
            frac <<= 1;
        }
    }

    /* scaled = UInt('1':fraction<51:44>) */
    scaled = deposit32(1 << 8, 0, 8, extract64(frac, 44, 8));
    estimate = recip_estimate(scaled);

    result_exp = exp_off - *exp;
    result_frac = deposit64(0, 44, 8, estimate);
    if (result_exp == 0) {
        result_frac = deposit64(result_frac >> 1, 51, 1, 1);
    } else if (result_exp == -1) {
        result_frac = deposit64(result_frac >> 2, 50, 2, 1);
        result_exp = 0;
    }

    *exp = result_exp;

    return result_frac;
}

static bool round_to_inf(float_status *fpst, bool sign_bit)
{
    switch (fpst->float_rounding_mode) {
    case float_round_nearest_even: /* Round to Nearest */
        return true;
    case float_round_up: /* Round to +Inf */
        return !sign_bit;
    case float_round_down: /* Round to -Inf */
        return sign_bit;
    case float_round_to_zero: /* Round to Zero */
        return false;
    default:
        break;
    }

    g_assert_not_reached();
    return false;
}

float16 HELPER(recpe_f16)(float16 input, void *fpstp)
{
    float_status *fpst = fpstp;
    float16 f16 = float16_squash_input_denormal(input, fpst);
    uint32_t f16_val = float16_val(f16);
    uint32_t f16_sign = float16_is_neg(f16);
    int f16_exp = extract32(f16_val, 10, 5);
    uint32_t f16_frac = extract32(f16_val, 0, 10);
    uint64_t f64_frac;

    if (float16_is_any_nan(f16)) {
        float16 nan = f16;
        if (float16_is_signaling_nan(f16, fpst)) {
            float_raise(float_flag_invalid, fpst);
            nan = float16_maybe_silence_nan(f16, fpst);
        }
        if (fpst->default_nan_mode) {
            nan =  float16_default_nan(fpst);
        }
        return nan;
    } else if (float16_is_infinity(f16)) {
        return float16_set_sign(float16_zero, float16_is_neg(f16));
    } else if (float16_is_zero(f16)) {
        float_raise(float_flag_divbyzero, fpst);
        return float16_set_sign(float16_infinity, float16_is_neg(f16));
    } else if (float16_abs(f16) < (1 << 8)) {
        /* Abs(value) < 2.0^-16 */
        float_raise(float_flag_overflow | float_flag_inexact, fpst);
        if (round_to_inf(fpst, f16_sign)) {
            return float16_set_sign(float16_infinity, f16_sign);
        } else {
            return float16_set_sign(float16_maxnorm, f16_sign);
        }
    } else if (f16_exp >= 29 && fpst->flush_to_zero) {
        float_raise(float_flag_underflow, fpst);
        return float16_set_sign(float16_zero, float16_is_neg(f16));
    }

    f64_frac = call_recip_estimate(&f16_exp, 29,
                                   ((uint64_t) f16_frac) << (52 - 10));

    /* result = sign : result_exp<4:0> : fraction<51:42> */
    f16_val = deposit32(0, 15, 1, f16_sign);
    f16_val = deposit32(f16_val, 10, 5, f16_exp);
    f16_val = deposit32(f16_val, 0, 10, extract64(f64_frac, 52 - 10, 10));
    return make_float16(f16_val);
}

float32 HELPER(recpe_f32)(float32 input, void *fpstp)
{
    float_status *fpst = fpstp;
    float32 f32 = float32_squash_input_denormal(input, fpst);
    uint32_t f32_val = float32_val(f32);
    bool f32_sign = float32_is_neg(f32);
    int f32_exp = extract32(f32_val, 23, 8);
    uint32_t f32_frac = extract32(f32_val, 0, 23);
    uint64_t f64_frac;

    if (float32_is_any_nan(f32)) {
        float32 nan = f32;
        if (float32_is_signaling_nan(f32, fpst)) {
            float_raise(float_flag_invalid, fpst);
            nan = float32_maybe_silence_nan(f32, fpst);
        }
        if (fpst->default_nan_mode) {
            nan =  float32_default_nan(fpst);
        }
        return nan;
    } else if (float32_is_infinity(f32)) {
        return float32_set_sign(float32_zero, float32_is_neg(f32));
    } else if (float32_is_zero(f32)) {
        float_raise(float_flag_divbyzero, fpst);
        return float32_set_sign(float32_infinity, float32_is_neg(f32));
    } else if (float32_abs(f32) < (1ULL << 21)) {
        /* Abs(value) < 2.0^-128 */
        float_raise(float_flag_overflow | float_flag_inexact, fpst);
        if (round_to_inf(fpst, f32_sign)) {
            return float32_set_sign(float32_infinity, f32_sign);
        } else {
            return float32_set_sign(float32_maxnorm, f32_sign);
        }
    } else if (f32_exp >= 253 && fpst->flush_to_zero) {
        float_raise(float_flag_underflow, fpst);
        return float32_set_sign(float32_zero, float32_is_neg(f32));
    }

    f64_frac = call_recip_estimate(&f32_exp, 253,
                                   ((uint64_t) f32_frac) << (52 - 23));

    /* result = sign : result_exp<7:0> : fraction<51:29> */
    f32_val = deposit32(0, 31, 1, f32_sign);
    f32_val = deposit32(f32_val, 23, 8, f32_exp);
    f32_val = deposit32(f32_val, 0, 23, extract64(f64_frac, 52 - 23, 23));
    return make_float32(f32_val);
}

float64 HELPER(recpe_f64)(float64 input, void *fpstp)
{
    float_status *fpst = fpstp;
    float64 f64 = float64_squash_input_denormal(input, fpst);
    uint64_t f64_val = float64_val(f64);
    bool f64_sign = float64_is_neg(f64);
    int f64_exp = extract64(f64_val, 52, 11);
    uint64_t f64_frac = extract64(f64_val, 0, 52);

    /* Deal with any special cases */
    if (float64_is_any_nan(f64)) {
        float64 nan = f64;
        if (float64_is_signaling_nan(f64, fpst)) {
            float_raise(float_flag_invalid, fpst);
            nan = float64_maybe_silence_nan(f64, fpst);
        }
        if (fpst->default_nan_mode) {
            nan =  float64_default_nan(fpst);
        }
        return nan;
    } else if (float64_is_infinity(f64)) {
        return float64_set_sign(float64_zero, float64_is_neg(f64));
    } else if (float64_is_zero(f64)) {
        float_raise(float_flag_divbyzero, fpst);
        return float64_set_sign(float64_infinity, float64_is_neg(f64));
    } else if ((f64_val & ~(1ULL << 63)) < (1ULL << 50)) {
        /* Abs(value) < 2.0^-1024 */
        float_raise(float_flag_overflow | float_flag_inexact, fpst);
        if (round_to_inf(fpst, f64_sign)) {
            return float64_set_sign(float64_infinity, f64_sign);
        } else {
            return float64_set_sign(float64_maxnorm, f64_sign);
        }
    } else if (f64_exp >= 2045 && fpst->flush_to_zero) {
        float_raise(float_flag_underflow, fpst);
        return float64_set_sign(float64_zero, float64_is_neg(f64));
    }

    f64_frac = call_recip_estimate(&f64_exp, 2045, f64_frac);

    /* result = sign : result_exp<10:0> : fraction<51:0>; */
    f64_val = deposit64(0, 63, 1, f64_sign);
    f64_val = deposit64(f64_val, 52, 11, f64_exp);
    f64_val = deposit64(f64_val, 0, 52, f64_frac);
    return make_float64(f64_val);
}

/* The algorithm that must be used to calculate the estimate
 * is specified by the ARM ARM.
 */

static int do_recip_sqrt_estimate(int a)
{
    int b, estimate;

    assert(128 <= a && a < 512);
    if (a < 256) {
        a = a * 2 + 1;
    } else {
        a = (a >> 1) << 1;
        a = (a + 1) * 2;
    }
    b = 512;
    while (a * (b + 1) * (b + 1) < (1 << 28)) {
        b += 1;
    }
    estimate = (b + 1) / 2;
    assert(256 <= estimate && estimate < 512);

    return estimate;
}


static uint64_t recip_sqrt_estimate(int *exp , int exp_off, uint64_t frac)
{
    int estimate;
    uint32_t scaled;

    if (*exp == 0) {
        while (extract64(frac, 51, 1) == 0) {
            frac = frac << 1;
            *exp -= 1;
        }
        frac = extract64(frac, 0, 51) << 1;
    }

    if (*exp & 1) {
        /* scaled = UInt('01':fraction<51:45>) */
        scaled = deposit32(1 << 7, 0, 7, extract64(frac, 45, 7));
    } else {
        /* scaled = UInt('1':fraction<51:44>) */
        scaled = deposit32(1 << 8, 0, 8, extract64(frac, 44, 8));
    }
    estimate = do_recip_sqrt_estimate(scaled);

    *exp = (exp_off - *exp) / 2;
    return extract64(estimate, 0, 8) << 44;
}

float16 HELPER(rsqrte_f16)(float16 input, void *fpstp)
{
    float_status *s = fpstp;
    float16 f16 = float16_squash_input_denormal(input, s);
    uint16_t val = float16_val(f16);
    bool f16_sign = float16_is_neg(f16);
    int f16_exp = extract32(val, 10, 5);
    uint16_t f16_frac = extract32(val, 0, 10);
    uint64_t f64_frac;

    if (float16_is_any_nan(f16)) {
        float16 nan = f16;
        if (float16_is_signaling_nan(f16, s)) {
            float_raise(float_flag_invalid, s);
            nan = float16_maybe_silence_nan(f16, s);
        }
        if (s->default_nan_mode) {
            nan =  float16_default_nan(s);
        }
        return nan;
    } else if (float16_is_zero(f16)) {
        float_raise(float_flag_divbyzero, s);
        return float16_set_sign(float16_infinity, f16_sign);
    } else if (f16_sign) {
        float_raise(float_flag_invalid, s);
        return float16_default_nan(s);
    } else if (float16_is_infinity(f16)) {
        return float16_zero;
    }

    /* Scale and normalize to a double-precision value between 0.25 and 1.0,
     * preserving the parity of the exponent.  */

    f64_frac = ((uint64_t) f16_frac) << (52 - 10);

    f64_frac = recip_sqrt_estimate(&f16_exp, 44, f64_frac);

    /* result = sign : result_exp<4:0> : estimate<7:0> : Zeros(2) */
    val = deposit32(0, 15, 1, f16_sign);
    val = deposit32(val, 10, 5, f16_exp);
    val = deposit32(val, 2, 8, extract64(f64_frac, 52 - 8, 8));
    return make_float16(val);
}

float32 HELPER(rsqrte_f32)(float32 input, void *fpstp)
{
    float_status *s = fpstp;
    float32 f32 = float32_squash_input_denormal(input, s);
    uint32_t val = float32_val(f32);
    uint32_t f32_sign = float32_is_neg(f32);
    int f32_exp = extract32(val, 23, 8);
    uint32_t f32_frac = extract32(val, 0, 23);
    uint64_t f64_frac;

    if (float32_is_any_nan(f32)) {
        float32 nan = f32;
        if (float32_is_signaling_nan(f32, s)) {
            float_raise(float_flag_invalid, s);
            nan = float32_maybe_silence_nan(f32, s);
        }
        if (s->default_nan_mode) {
            nan =  float32_default_nan(s);
        }
        return nan;
    } else if (float32_is_zero(f32)) {
        float_raise(float_flag_divbyzero, s);
        return float32_set_sign(float32_infinity, float32_is_neg(f32));
    } else if (float32_is_neg(f32)) {
        float_raise(float_flag_invalid, s);
        return float32_default_nan(s);
    } else if (float32_is_infinity(f32)) {
        return float32_zero;
    }

    /* Scale and normalize to a double-precision value between 0.25 and 1.0,
     * preserving the parity of the exponent.  */

    f64_frac = ((uint64_t) f32_frac) << 29;

    f64_frac = recip_sqrt_estimate(&f32_exp, 380, f64_frac);

    /* result = sign : result_exp<4:0> : estimate<7:0> : Zeros(15) */
    val = deposit32(0, 31, 1, f32_sign);
    val = deposit32(val, 23, 8, f32_exp);
    val = deposit32(val, 15, 8, extract64(f64_frac, 52 - 8, 8));
    return make_float32(val);
}

float64 HELPER(rsqrte_f64)(float64 input, void *fpstp)
{
    float_status *s = fpstp;
    float64 f64 = float64_squash_input_denormal(input, s);
    uint64_t val = float64_val(f64);
    bool f64_sign = float64_is_neg(f64);
    int f64_exp = extract64(val, 52, 11);
    uint64_t f64_frac = extract64(val, 0, 52);

    if (float64_is_any_nan(f64)) {
        float64 nan = f64;
        if (float64_is_signaling_nan(f64, s)) {
            float_raise(float_flag_invalid, s);
            nan = float64_maybe_silence_nan(f64, s);
        }
        if (s->default_nan_mode) {
            nan =  float64_default_nan(s);
        }
        return nan;
    } else if (float64_is_zero(f64)) {
        float_raise(float_flag_divbyzero, s);
        return float64_set_sign(float64_infinity, float64_is_neg(f64));
    } else if (float64_is_neg(f64)) {
        float_raise(float_flag_invalid, s);
        return float64_default_nan(s);
    } else if (float64_is_infinity(f64)) {
        return float64_zero;
    }

    f64_frac = recip_sqrt_estimate(&f64_exp, 3068, f64_frac);

    /* result = sign : result_exp<4:0> : estimate<7:0> : Zeros(44) */
    val = deposit64(0, 61, 1, f64_sign);
    val = deposit64(val, 52, 11, f64_exp);
    val = deposit64(val, 44, 8, extract64(f64_frac, 52 - 8, 8));
    return make_float64(val);
}

uint32_t HELPER(recpe_u32)(uint32_t a, void *fpstp)
{
    /* float_status *s = fpstp; */
    int input, estimate;

    if ((a & 0x80000000) == 0) {
        return 0xffffffff;
    }

    input = extract32(a, 23, 9);
    estimate = recip_estimate(input);

    return deposit32(0, (32 - 9), 9, estimate);
}

uint32_t HELPER(rsqrte_u32)(uint32_t a, void *fpstp)
{
    int estimate;

    if ((a & 0xc0000000) == 0) {
        return 0xffffffff;
    }

    estimate = do_recip_sqrt_estimate(extract32(a, 23, 9));

    return deposit32(0, 23, 9, estimate);
}

/* VFPv4 fused multiply-accumulate */
float32 VFP_HELPER(muladd, s)(float32 a, float32 b, float32 c, void *fpstp)
{
    float_status *fpst = fpstp;
    return float32_muladd(a, b, c, 0, fpst);
}

float64 VFP_HELPER(muladd, d)(float64 a, float64 b, float64 c, void *fpstp)
{
    float_status *fpst = fpstp;
    return float64_muladd(a, b, c, 0, fpst);
}

/* ARMv8 round to integral */
float32 HELPER(rints_exact)(float32 x, void *fp_status)
{
    return float32_round_to_int(x, fp_status);
}

float64 HELPER(rintd_exact)(float64 x, void *fp_status)
{
    return float64_round_to_int(x, fp_status);
}

float32 HELPER(rints)(float32 x, void *fp_status)
{
    int old_flags = get_float_exception_flags(fp_status), new_flags;
    float32 ret;

    ret = float32_round_to_int(x, fp_status);

    /* Suppress any inexact exceptions the conversion produced */
    if (!(old_flags & float_flag_inexact)) {
        new_flags = get_float_exception_flags(fp_status);
        set_float_exception_flags(new_flags & ~float_flag_inexact, fp_status);
    }

    return ret;
}

float64 HELPER(rintd)(float64 x, void *fp_status)
{
    int old_flags = get_float_exception_flags(fp_status), new_flags;
    float64 ret;

    ret = float64_round_to_int(x, fp_status);

    new_flags = get_float_exception_flags(fp_status);

    /* Suppress any inexact exceptions the conversion produced */
    if (!(old_flags & float_flag_inexact)) {
        new_flags = get_float_exception_flags(fp_status);
        set_float_exception_flags(new_flags & ~float_flag_inexact, fp_status);
    }

    return ret;
}

/* Convert ARM rounding mode to softfloat */
int arm_rmode_to_sf(int rmode)
{
    switch (rmode) {
    case FPROUNDING_TIEAWAY:
        rmode = float_round_ties_away;
        break;
    case FPROUNDING_ODD:
        /* FIXME: add support for TIEAWAY and ODD */
        qemu_log_mask(LOG_UNIMP, "arm: unimplemented rounding mode: %d\n",
                      rmode);
    case FPROUNDING_TIEEVEN:
    default:
        rmode = float_round_nearest_even;
        break;
    case FPROUNDING_POSINF:
        rmode = float_round_up;
        break;
    case FPROUNDING_NEGINF:
        rmode = float_round_down;
        break;
    case FPROUNDING_ZERO:
        rmode = float_round_to_zero;
        break;
    }
    return rmode;
}

/* CRC helpers.
 * The upper bytes of val (above the number specified by 'bytes') must have
 * been zeroed out by the caller.
 */
uint32_t HELPER(crc32_arm)(uint32_t acc, uint32_t val, uint32_t bytes)
{
#if 0   // FIXME
    uint8_t buf[4];

    stl_le_p(buf, val);

    /* zlib crc32 converts the accumulator and output to one's complement.  */
    return crc32(acc ^ 0xffffffff, buf, bytes) ^ 0xffffffff;
#endif
    return 0;
}

uint32_t HELPER(crc32c)(uint32_t acc, uint32_t val, uint32_t bytes)
{
    uint8_t buf[4];

    stl_le_p(buf, val);

    /* Linux crc32c converts the output to one's complement.  */
    return crc32c(acc, buf, bytes) ^ 0xffffffff;
}

/* Return the exception level to which FP-disabled exceptions should
 * be taken, or 0 if FP is enabled.
 */
static inline int fp_exception_el(CPUARMState *env)
{
#ifndef CONFIG_USER_ONLY
    int fpen;
    int cur_el = arm_current_el(env);

    /* CPACR and the CPTR registers don't exist before v6, so FP is
     * always accessible
     */
    if (!arm_feature(env, ARM_FEATURE_V6)) {
        return 0;
    }

    /* The CPACR controls traps to EL1, or PL1 if we're 32 bit:
     * 0, 2 : trap EL0 and EL1/PL1 accesses
     * 1    : trap only EL0 accesses
     * 3    : trap no accesses
     */
    fpen = extract32(env->cp15.cpacr_el1, 20, 2);
    switch (fpen) {
    case 0:
    case 2:
        if (cur_el == 0 || cur_el == 1) {
            /* Trap to PL1, which might be EL1 or EL3 */
            if (arm_is_secure(env) && !arm_el_is_aa64(env, 3)) {
                return 3;
            }
            return 1;
        }
        if (cur_el == 3 && !is_a64(env)) {
            /* Secure PL1 running at EL3 */
            return 3;
        }
        break;
    case 1:
        if (cur_el == 0) {
            return 1;
        }
        break;
    case 3:
        break;
    }

    /* For the CPTR registers we don't need to guard with an ARM_FEATURE
     * check because zero bits in the registers mean "don't trap".
     */

    /* CPTR_EL2 : present in v7VE or v8 */
    if (cur_el <= 2 && extract32(env->cp15.cptr_el[2], 10, 1)
        && !arm_is_secure_below_el3(env)) {
        /* Trap FP ops at EL2, NS-EL1 or NS-EL0 to EL2 */
        return 2;
    }

    /* CPTR_EL3 : present in v8 */
    if (extract32(env->cp15.cptr_el[3], 10, 1)) {
        /* Trap all FP ops to EL3 */
        return 3;
    }
#endif
    return 0;
}

void cpu_get_tb_cpu_state(CPUARMState *env, target_ulong *pc,
                          target_ulong *cs_base, uint32_t *pflags)
{
    ARMMMUIdx mmu_idx = core_to_arm_mmu_idx(env, cpu_mmu_index(env, false));
    int fp_el = fp_exception_el(env);
    uint32_t flags;

    if (is_a64(env)) {
        int sve_el = sve_exception_el(env);
        uint32_t zcr_len;

        *pc = env->pc;
        flags = ARM_TBFLAG_AARCH64_STATE_MASK;
        /* Get control bits for tagged addresses */
        flags |= (arm_regime_tbi0(env, mmu_idx) << ARM_TBFLAG_TBI0_SHIFT);
        flags |= (arm_regime_tbi1(env, mmu_idx) << ARM_TBFLAG_TBI1_SHIFT);
        flags |= sve_el << ARM_TBFLAG_SVEEXC_EL_SHIFT;

        /* If SVE is disabled, but FP is enabled,
           then the effective len is 0.  */
        if (sve_el != 0 && fp_el == 0) {
            zcr_len = 0;
        } else {
            int current_el = arm_current_el(env);

            zcr_len = env->vfp.zcr_el[current_el <= 1 ? 1 : current_el];
            zcr_len &= 0xf;
            if (current_el < 2 && arm_feature(env, ARM_FEATURE_EL2)) {
                zcr_len = MIN(zcr_len, 0xf & (uint32_t)env->vfp.zcr_el[2]);
            }
            if (current_el < 3 && arm_feature(env, ARM_FEATURE_EL3)) {
                zcr_len = MIN(zcr_len, 0xf & (uint32_t)env->vfp.zcr_el[3]);
            }
        }
        flags |= zcr_len << ARM_TBFLAG_ZCR_LEN_SHIFT;
    } else {
        *pc = env->regs[15];
        flags = (env->thumb << ARM_TBFLAG_THUMB_SHIFT)
            | (env->vfp.vec_len << ARM_TBFLAG_VECLEN_SHIFT)
            | (env->vfp.vec_stride << ARM_TBFLAG_VECSTRIDE_SHIFT)
            | (env->condexec_bits << ARM_TBFLAG_CONDEXEC_SHIFT)
            | (arm_sctlr_b(env) << ARM_TBFLAG_SCTLR_B_SHIFT);
        if (!(access_secure_reg(env))) {
            flags |= ARM_TBFLAG_NS_MASK;
        }
        if (env->vfp.xregs[ARM_VFP_FPEXC] & (1 << 30)
            || arm_el_is_aa64(env, 1)) {
            flags |= ARM_TBFLAG_VFPEN_MASK;
        }
        flags |= (extract32(env->cp15.c15_cpar, 0, 2)
                  << ARM_TBFLAG_XSCALE_CPAR_SHIFT);
    }

    flags |= (arm_to_core_mmu_idx(mmu_idx) << ARM_TBFLAG_MMUIDX_SHIFT);

    /* The SS_ACTIVE and PSTATE_SS bits correspond to the state machine
     * states defined in the ARM ARM for software singlestep:
     *  SS_ACTIVE   PSTATE.SS   State
     *     0            x       Inactive (the TB flag for SS is always 0)
     *     1            0       Active-pending
     *     1            1       Active-not-pending
     */
    if (arm_singlestep_active(env)) {
        flags |= ARM_TBFLAG_SS_ACTIVE_MASK;
        if (is_a64(env)) {
            if (env->pstate & PSTATE_SS) {
                flags |= ARM_TBFLAG_PSTATE_SS_MASK;
            }
        } else {
            if (env->uncached_cpsr & PSTATE_SS) {
                flags |= ARM_TBFLAG_PSTATE_SS_MASK;
            }
        }
    }
    if (arm_cpu_data_is_big_endian(env)) {
        flags |= ARM_TBFLAG_BE_DATA_MASK;
    }
    flags |= fp_el << ARM_TBFLAG_FPEXC_EL_SHIFT;

    if (arm_v7m_is_handler_mode(env)) {
        flags |= ARM_TBFLAG_HANDLER_MASK;
    }

    *pflags = flags;
    *cs_base = 0;
}
