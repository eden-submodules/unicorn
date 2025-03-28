/*
 * ARM virtual CPU header
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

#ifndef ARM_CPU_H
#define ARM_CPU_H

#include "config.h"

#include "kvm-consts.h"
#include "hw/registerfields.h"

#if defined(TARGET_AARCH64)
  /* AArch64 definitions */
#  define TARGET_LONG_BITS 64
#else
#  define TARGET_LONG_BITS 32
#endif

#define TARGET_IS_BIENDIAN 1

/* ARM processors have a weak memory model */
#define TCG_GUEST_DEFAULT_MO      (0)

#define CPUArchState struct CPUARMState

#include "qemu-common.h"
#include "cpu-qom.h"
#include "exec/cpu-defs.h"

#define EXCP_UDEF            1   /* undefined instruction */
#define EXCP_SWI             2   /* software interrupt */
#define EXCP_PREFETCH_ABORT  3
#define EXCP_DATA_ABORT      4
#define EXCP_IRQ             5
#define EXCP_FIQ             6
#define EXCP_BKPT            7
#define EXCP_EXCEPTION_EXIT  8   /* Return from v7M exception.  */
#define EXCP_KERNEL_TRAP     9   /* Jumped to kernel code page.  */
#define EXCP_HVC            11   /* HyperVisor Call */
#define EXCP_HYP_TRAP       12
#define EXCP_SMC            13   /* Secure Monitor Call */
#define EXCP_VIRQ           14
#define EXCP_VFIQ           15
#define EXCP_SEMIHOST       16   /* semihosting call */
#define EXCP_NOCP           17   /* v7M NOCP UsageFault */
#define EXCP_INVSTATE       18   /* v7M INVSTATE UsageFault */
/* NB: add new EXCP_ defines to the array in arm_log_exception() too */

#define ARMV7M_EXCP_RESET   1
#define ARMV7M_EXCP_NMI     2
#define ARMV7M_EXCP_HARD    3
#define ARMV7M_EXCP_MEM     4
#define ARMV7M_EXCP_BUS     5
#define ARMV7M_EXCP_USAGE   6
#define ARMV7M_EXCP_SECURE  7
#define ARMV7M_EXCP_SVC     11
#define ARMV7M_EXCP_DEBUG   12
#define ARMV7M_EXCP_PENDSV  14
#define ARMV7M_EXCP_SYSTICK 15

/* For M profile, some registers are banked secure vs non-secure;
 * these are represented as a 2-element array where the first element
 * is the non-secure copy and the second is the secure copy.
 * When the CPU does not have implement the security extension then
 * only the first element is used.
 * This means that the copy for the current security state can be
 * accessed via env->registerfield[env->v7m.secure] (whether the security
 * extension is implemented or not).
 */
enum {
    M_REG_NS = 0,
    M_REG_S = 1,
    M_REG_NUM_BANKS = 2,
};

/* ARM-specific interrupt pending bits.  */
#define CPU_INTERRUPT_FIQ   CPU_INTERRUPT_TGT_EXT_1
#define CPU_INTERRUPT_VIRQ  CPU_INTERRUPT_TGT_EXT_2
#define CPU_INTERRUPT_VFIQ  CPU_INTERRUPT_TGT_EXT_3

/* The usual mapping for an AArch64 system register to its AArch32
 * counterpart is for the 32 bit world to have access to the lower
 * half only (with writes leaving the upper half untouched). It's
 * therefore useful to be able to pass TCG the offset of the least
 * significant half of a uint64_t struct member.
 */
#ifdef HOST_WORDS_BIGENDIAN
#define offsetoflow32(S, M) (offsetof(S, M) + sizeof(uint32_t))
#define offsetofhigh32(S, M) offsetof(S, M)
#else
#define offsetoflow32(S, M) offsetof(S, M)
#define offsetofhigh32(S, M) (offsetof(S, M) + sizeof(uint32_t))
#endif

/* Meanings of the ARMCPU object's four inbound GPIO lines */
#define ARM_CPU_IRQ 0
#define ARM_CPU_FIQ 1
#define ARM_CPU_VIRQ 2
#define ARM_CPU_VFIQ 3

#define NB_MMU_MODES 8
/* ARM-specific extra insn start words:
 * 1: Conditional execution bits
 * 2: Partial exception syndrome for data aborts
 */
#define TARGET_INSN_START_EXTRA_WORDS 2

/* The 2nd extra word holding syndrome info for data aborts does not use
 * the upper 6 bits nor the lower 14 bits. We mask and shift it down to
 * help the sleb128 encoder do a better job.
 * When restoring the CPU state, we shift it back up.
 */
#define ARM_INSN_START_WORD2_MASK ((1 << 26) - 1)
#define ARM_INSN_START_WORD2_SHIFT 14

/* We currently assume float and double are IEEE single and double
   precision respectively.
   Doing runtime conversions is tricky because VFP registers may contain
   integer values (eg. as the result of a FTOSI instruction).
   s<2n> maps to the least significant half of d<n>
   s<2n+1> maps to the most significant half of d<n>
 */

/* CPU state for each instance of a generic timer (in cp15 c14) */
typedef struct ARMGenericTimer {
    uint64_t cval; /* Timer CompareValue register */
    uint64_t ctl; /* Timer Control register */
} ARMGenericTimer;

#define GTIMER_PHYS 0
#define GTIMER_VIRT 1
#define GTIMER_HYP  2
#define GTIMER_SEC  3
#define NUM_GTIMERS 4

typedef struct {
    uint64_t raw_tcr;
    uint32_t mask;
    uint32_t base_mask;
} TCR;

/* Define a maximum sized vector register.
 * For 32-bit, this is a 128-bit NEON/AdvSIMD register.
 * For 64-bit, this is a 2048-bit SVE register.
 *
 * Note that the mapping between S, D, and Q views of the register bank
 * differs between AArch64 and AArch32.
 * In AArch32:
 *  Qn = regs[n].d[1]:regs[n].d[0]
 *  Dn = regs[n / 2].d[n & 1]
 *  Sn = regs[n / 4].d[n % 4 / 2],
 *       bits 31..0 for even n, and bits 63..32 for odd n
 *       (and regs[16] to regs[31] are inaccessible)
 * In AArch64:
 *  Zn = regs[n].d[*]
 *  Qn = regs[n].d[1]:regs[n].d[0]
 *  Dn = regs[n].d[0]
 *  Sn = regs[n].d[0] bits 31..0
 *  Hn = regs[n].d[0] bits 15..0
 *
 * This corresponds to the architecturally defined mapping between
 * the two execution states, and means we do not need to explicitly
 * map these registers when changing states.
 *
 * Align the data for use with TCG host vector operations.
 */

#ifdef TARGET_AARCH64
# define ARM_MAX_VQ    16
#else
# define ARM_MAX_VQ    1
#endif

typedef struct ARMVectorReg {
    uint64_t QEMU_ALIGNED(16, d[2 * ARM_MAX_VQ]);
} ARMVectorReg;

/* In AArch32 mode, predicate registers do not exist at all.  */
#ifdef TARGET_AARCH64
typedef struct ARMPredicateReg {
    uint64_t QEMU_ALIGNED(16, p[2 * ARM_MAX_VQ / 8]);
} ARMPredicateReg;
#endif


typedef struct CPUARMState {
    /* Regs for current mode.  */
    uint32_t regs[16];

    /* 32/64 switch only happens when taking and returning from
     * exceptions so the overlap semantics are taken care of then
     * instead of having a complicated union.
     */
    /* Regs for A64 mode.  */
    uint64_t xregs[32];
    uint64_t pc;
    /* PSTATE isn't an architectural register for ARMv8. However, it is
     * convenient for us to assemble the underlying state into a 32 bit format
     * identical to the architectural format used for the SPSR. (This is also
     * what the Linux kernel's 'pstate' field in signal handlers and KVM's
     * 'pstate' register are.) Of the PSTATE bits:
     *  NZCV are kept in the split out env->CF/VF/NF/ZF, (which have the same
     *    semantics as for AArch32, as described in the comments on each field)
     *  nRW (also known as M[4]) is kept, inverted, in env->aarch64
     *  DAIF (exception masks) are kept in env->daif
     *  all other bits are stored in their correct places in env->pstate
     */
    uint32_t pstate;
    uint32_t aarch64; /* 1 if CPU is in aarch64 state; inverse of PSTATE.nRW */

    /* Frequently accessed CPSR bits are stored separately for efficiency.
       This contains all the other bits.  Use cpsr_{read,write} to access
       the whole CPSR.  */
    uint32_t uncached_cpsr;
    uint32_t spsr;

    /* Banked registers.  */
    uint64_t banked_spsr[8];
    uint32_t banked_r13[8];
    uint32_t banked_r14[8];

    /* These hold r8-r12.  */
    uint32_t usr_regs[5];
    uint32_t fiq_regs[5];

    /* cpsr flag cache for faster execution */
    uint32_t CF; /* 0 or 1 */
    uint32_t VF; /* V is the bit 31. All other bits are undefined */
    uint32_t NF; /* N is bit 31. All other bits are undefined.  */
    uint32_t ZF; /* Z set if zero.  */
    uint32_t QF; /* 0 or 1 */
    uint32_t GE; /* cpsr[19:16] */
    uint32_t thumb; /* cpsr[5]. 0 = arm mode, 1 = thumb mode. */
    uint32_t condexec_bits; /* IT bits.  cpsr[15:10,26:25].  */
    uint64_t daif; /* exception masks, in the bits they are in PSTATE */

    uint64_t elr_el[4]; /* AArch64 exception link regs  */
    uint64_t sp_el[4]; /* AArch64 banked stack pointers */

    /* System control coprocessor (cp15) */
    struct {
        uint32_t c0_cpuid;
        union { /* Cache size selection */
            struct {
                uint64_t _unused_csselr0;
                uint64_t csselr_ns;
                uint64_t _unused_csselr1;
                uint64_t csselr_s;
            };
            uint64_t csselr_el[4];
        };
        union { /* System control register. */
            struct {
                uint64_t _unused_sctlr;
                uint64_t sctlr_ns;
                uint64_t hsctlr;
                uint64_t sctlr_s;
            };
            uint64_t sctlr_el[4];
        };
        uint64_t cpacr_el1; /* Architectural feature access control register */
        uint64_t cptr_el[4];  /* ARMv8 feature trap registers */
        uint32_t c1_xscaleauxcr; /* XScale auxiliary control register.  */
        uint64_t sder; /* Secure debug enable register. */
        uint32_t nsacr; /* Non-secure access control register. */
        union { /* MMU translation table base 0. */
            struct {
                uint64_t _unused_ttbr0_0;
                uint64_t ttbr0_ns;
                uint64_t _unused_ttbr0_1;
                uint64_t ttbr0_s;
            };
            uint64_t ttbr0_el[4];
        };
        union { /* MMU translation table base 1. */
            struct {
                uint64_t _unused_ttbr1_0;
                uint64_t ttbr1_ns;
                uint64_t _unused_ttbr1_1;
                uint64_t ttbr1_s;
            };
            uint64_t ttbr1_el[4];
        };
        uint64_t vttbr_el2; /* Virtualization Translation Table Base.  */
        /* MMU translation table base control. */
        TCR tcr_el[4];
        TCR vtcr_el2; /* Virtualization Translation Control.  */
        uint32_t c2_data; /* MPU data cacheable bits.  */
        uint32_t c2_insn; /* MPU instruction cacheable bits.  */
        union { /* MMU domain access control register
                 * MPU write buffer control.
                 */
            struct {
                uint64_t dacr_ns;
                uint64_t dacr_s;
            };
            struct {
                uint64_t dacr32_el2;
            };
        };
        uint32_t pmsav5_data_ap; /* PMSAv5 MPU data access permissions */
        uint32_t pmsav5_insn_ap; /* PMSAv5 MPU insn access permissions */
        uint64_t hcr_el2; /* Hypervisor configuration register */
        uint64_t scr_el3; /* Secure configuration register.  */
        union { /* Fault status registers.  */
            struct {
                uint64_t ifsr_ns;
                uint64_t ifsr_s;
            };
            struct {
                uint64_t ifsr32_el2;
            };
        };
        union {
            struct {
                uint64_t _unused_dfsr;
                uint64_t dfsr_ns;
                uint64_t hsr;
                uint64_t dfsr_s;
            };
            uint64_t esr_el[4];
        };
        uint32_t c6_region[8]; /* MPU base/size registers.  */
        union { /* Fault address registers. */
            struct {
                uint64_t _unused_far0;
#ifdef HOST_WORDS_BIGENDIAN
                uint32_t ifar_ns;
                uint32_t dfar_ns;
                uint32_t ifar_s;
                uint32_t dfar_s;
#else
                uint32_t dfar_ns;
                uint32_t ifar_ns;
                uint32_t dfar_s;
                uint32_t ifar_s;
#endif
                uint64_t _unused_far3;
            };
            uint64_t far_el[4];
        };
        uint64_t hpfar_el2;
        uint64_t hstr_el2;
        union { /* Translation result. */
            struct {
                uint64_t _unused_par_0;
                uint64_t par_ns;
                uint64_t _unused_par_1;
                uint64_t par_s;
            };
            uint64_t par_el[4];
        };

        uint32_t c9_insn; /* Cache lockdown registers.  */
        uint32_t c9_data;
        uint64_t c9_pmcr; /* performance monitor control register */
        uint64_t c9_pmcnten; /* perf monitor counter enables */
        uint32_t c9_pmovsr; /* perf monitor overflow status */
        uint32_t c9_pmuserenr; /* perf monitor user enable */
        uint64_t c9_pmselr; /* perf monitor counter selection register */
        uint64_t c9_pminten; /* perf monitor interrupt enables */
        union { /* Memory attribute redirection */
            struct {
#ifdef HOST_WORDS_BIGENDIAN
                uint64_t _unused_mair_0;
                uint32_t mair1_ns;
                uint32_t mair0_ns;
                uint64_t _unused_mair_1;
                uint32_t mair1_s;
                uint32_t mair0_s;
#else
                uint64_t _unused_mair_0;
                uint32_t mair0_ns;
                uint32_t mair1_ns;
                uint64_t _unused_mair_1;
                uint32_t mair0_s;
                uint32_t mair1_s;
#endif
            };
            uint64_t mair_el[4];
        };
        union { /* vector base address register */
            struct {
                uint64_t _unused_vbar;
                uint64_t vbar_ns;
                uint64_t hvbar;
                uint64_t vbar_s;
            };
            uint64_t vbar_el[4];
        };
        uint32_t mvbar; /* (monitor) vector base address register */
        struct { /* FCSE PID. */
            uint32_t fcseidr_ns;
            uint32_t fcseidr_s;
        };
        union { /* Context ID. */
            struct {
                uint64_t _unused_contextidr_0;
                uint64_t contextidr_ns;
                uint64_t _unused_contextidr_1;
                uint64_t contextidr_s;
            };
            uint64_t contextidr_el[4];
        };
        union { /* User RW Thread register. */
            struct {
                uint64_t tpidrurw_ns;
                uint64_t tpidrprw_ns;
                uint64_t htpidr;
                uint64_t _tpidr_el3;
            };
            uint64_t tpidr_el[4];
        };
        /* The secure banks of these registers don't map anywhere */
        uint64_t tpidrurw_s;
        uint64_t tpidrprw_s;
        uint64_t tpidruro_s;

        union { /* User RO Thread register. */
            uint64_t tpidruro_ns;
            uint64_t tpidrro_el[1];
        };
        uint64_t c14_cntfrq; /* Counter Frequency register */
        uint64_t c14_cntkctl; /* Timer Control register */
        uint32_t cnthctl_el2; /* Counter/Timer Hyp Control register */
        uint64_t cntvoff_el2; /* Counter Virtual Offset register */
        ARMGenericTimer c14_timer[NUM_GTIMERS];
        uint32_t c15_cpar; /* XScale Coprocessor Access Register */
        uint32_t c15_ticonfig; /* TI925T configuration byte.  */
        uint32_t c15_i_max; /* Maximum D-cache dirty line index.  */
        uint32_t c15_i_min; /* Minimum D-cache dirty line index.  */
        uint32_t c15_threadid; /* TI debugger thread-ID.  */
        uint32_t c15_config_base_address; /* SCU base address.  */
        uint32_t c15_diagnostic; /* diagnostic register */
        uint32_t c15_power_diagnostic;
        uint32_t c15_power_control; /* power control */
        uint64_t dbgbvr[16]; /* breakpoint value registers */
        uint64_t dbgbcr[16]; /* breakpoint control registers */
        uint64_t dbgwvr[16]; /* watchpoint value registers */
        uint64_t dbgwcr[16]; /* watchpoint control registers */
        uint64_t mdscr_el1;
        uint64_t oslsr_el1; /* OS Lock Status */
        uint64_t mdcr_el2;
        uint64_t mdcr_el3;
        /* If the counter is enabled, this stores the last time the counter
         * was reset. Otherwise it stores the counter value
         */
        uint64_t c15_ccnt;
        uint64_t pmccfiltr_el0; /* Performance Monitor Filter Register */
        uint64_t vpidr_el2; /* Virtualization Processor ID Register */
        uint64_t vmpidr_el2; /* Virtualization Multiprocessor ID Register */
    } cp15;

    struct {
        /* M profile has up to 4 stack pointers:
         * a Main Stack Pointer and a Process Stack Pointer for each
         * of the Secure and Non-Secure states. (If the CPU doesn't support
         * the security extension then it has only two SPs.)
         * In QEMU we always store the currently active SP in regs[13],
         * and the non-active SP for the current security state in
         * v7m.other_sp. The stack pointers for the inactive security state
         * are stored in other_ss_msp and other_ss_psp.
         * switch_v7m_security_state() is responsible for rearranging them
         * when we change security state.
         */
        uint32_t other_sp;
        uint32_t other_ss_msp;
        uint32_t other_ss_psp;
        uint32_t vecbase[M_REG_NUM_BANKS];
        uint32_t basepri[M_REG_NUM_BANKS];
        uint32_t control[M_REG_NUM_BANKS];
        uint32_t ccr[M_REG_NUM_BANKS]; /* Configuration and Control */
        uint32_t cfsr[M_REG_NUM_BANKS]; /* Configurable Fault Status */
        uint32_t hfsr; /* HardFault Status */
        uint32_t dfsr; /* Debug Fault Status Register */
        uint32_t sfsr; /* Secure Fault Status Register */
        uint32_t mmfar[M_REG_NUM_BANKS]; /* MemManage Fault Address */
        uint32_t bfar; /* BusFault Address */
        uint32_t sfar; /* Secure Fault Address Register */
        unsigned mpu_ctrl[M_REG_NUM_BANKS]; /* MPU_CTRL */
        int exception;
        uint32_t primask[M_REG_NUM_BANKS];
        uint32_t faultmask[M_REG_NUM_BANKS];
        uint32_t aircr; /* only holds r/w state if security extn implemented */
        uint32_t secure; /* Is CPU in Secure state? (not guest visible) */
        uint32_t csselr[M_REG_NUM_BANKS];
        uint32_t scr[M_REG_NUM_BANKS];
        uint32_t msplim[M_REG_NUM_BANKS];
        uint32_t psplim[M_REG_NUM_BANKS];
    } v7m;

    /* Information associated with an exception about to be taken:
     * code which raises an exception must set cs->exception_index and
     * the relevant parts of this structure; the cpu_do_interrupt function
     * will then set the guest-visible registers as part of the exception
     * entry process.
     */
    struct {
        uint32_t syndrome; /* AArch64 format syndrome register */
        uint32_t fsr; /* AArch32 format fault status register info */
        uint64_t vaddress; /* virtual addr associated with exception, if any */
        uint32_t target_el; /* EL the exception should be targeted for */
        /* If we implement EL2 we will also need to store information
         * about the intermediate physical address for stage 2 faults.
         */
    } exception;

    /* Thumb-2 EE state.  */
    uint32_t teecr;
    uint32_t teehbr;

    /* VFP coprocessor state.  */
    struct {
        ARMVectorReg zregs[32];

#ifdef TARGET_AARCH64
        /* Store FFR as pregs[16] to make it easier to treat as any other.  */
        ARMPredicateReg pregs[17];
#endif

        uint32_t xregs[16];
        /* We store these fpcsr fields separately for convenience.  */
        int vec_len;
        int vec_stride;

        /* scratch space when Tn are not sufficient.  */
        uint32_t scratch[8];

        /* There are a number of distinct float control structures:
         *
         *  fp_status: is the "normal" fp status.
         *  fp_status_fp16: used for half-precision calculations
         *  standard_fp_status : the ARM "Standard FPSCR Value"
         *
         * Half-precision operations are governed by a separate
         * flush-to-zero control bit in FPSCR:FZ16. We pass a separate
         * status structure to control this.
         *
         * The "Standard FPSCR", ie default-NaN, flush-to-zero,
         * round-to-nearest and is used by any operations (generally
         * Neon) which the architecture defines as controlled by the
         * standard FPSCR value rather than the FPSCR.
         *
         * To avoid having to transfer exception bits around, we simply
         * say that the FPSCR cumulative exception flags are the logical
         * OR of the flags in the three fp statuses. This relies on the
         * only thing which needs to read the exception flags being
         * an explicit FPSCR read.
         */
        float_status fp_status;
        float_status fp_status_f16;
        float_status standard_fp_status;

        /* ZCR_EL[1-3] */
        uint64_t zcr_el[4];
    } vfp;
    uint64_t exclusive_addr;
    uint64_t exclusive_val;
    uint64_t exclusive_high;

    /* iwMMXt coprocessor state.  */
    struct {
        uint64_t regs[16];
        uint64_t val;

        uint32_t cregs[16];
    } iwmmxt;

#if defined(CONFIG_USER_ONLY)
    /* For usermode syscall translation.  */
    int eabi;
#endif

    struct CPUBreakpoint *cpu_breakpoint[16];
    struct CPUWatchpoint *cpu_watchpoint[16];

    /* Fields up to this point are cleared by a CPU reset */
    struct {} end_reset_fields;

    CPU_COMMON

    /* Fields after CPU_COMMON are preserved across CPU reset. */

    /* Internal CPU feature flags.  */
    uint64_t features;

    /* PMSAv7 MPU */
    struct {
        uint32_t *drbar;
        uint32_t *drsr;
        uint32_t *dracr;
        uint32_t rnr[M_REG_NUM_BANKS];
    } pmsav7;

    /* PMSAv8 MPU */
    struct {
        /* The PMSAv8 implementation also shares some PMSAv7 config
         * and state:
         *  pmsav7.rnr (region number register)
         *  pmsav7_dregion (number of configured regions)
         */
        uint32_t *rbar[M_REG_NUM_BANKS];
        uint32_t *rlar[M_REG_NUM_BANKS];
        uint32_t mair0[M_REG_NUM_BANKS];
        uint32_t mair1[M_REG_NUM_BANKS];
    } pmsav8;

    /* v8M SAU */
    struct {
        uint32_t *rbar;
        uint32_t *rlar;
        uint32_t rnr;
        uint32_t ctrl;
    } sau;

    void *nvic;
    const struct arm_boot_info *boot_info;
    /* Store GICv3CPUState to access from this struct */
    void *gicv3state;

    // Unicorn engine
    struct uc_struct *uc;
} CPUARMState;

/**
 * ARMELChangeHook:
 * type of a function which can be registered via arm_register_el_change_hook()
 * to get callbacks when the CPU changes its exception level or mode.
 */
typedef void ARMELChangeHook(ARMCPU *cpu, void *opaque);


/* These values map onto the return values for
 * QEMU_PSCI_0_2_FN_AFFINITY_INFO */
typedef enum ARMPSCIState {
    PSCI_ON = 0,
    PSCI_OFF = 1,
    PSCI_ON_PENDING = 2
} ARMPSCIState;

/**
 * ARMCPU:
 * @env: #CPUARMState
 *
 * An ARM CPU core.
 */
typedef struct ARMCPU {
    /*< private >*/
    CPUState parent_obj;
    /*< public >*/

    CPUARMState env;

    /* Coprocessor information */
    GHashTable *cp_regs;
    /* For marshalling (mostly coprocessor) register state between the
     * kernel and QEMU (for KVM) and between two QEMUs (for migration),
     * we use these arrays.
     */
    /* List of register indexes managed via these arrays; (full KVM style
     * 64 bit indexes, not CPRegInfo 32 bit indexes)
     */
    uint64_t *cpreg_indexes;
    /* Values of the registers (cpreg_indexes[i]'s value is cpreg_values[i]) */
    uint64_t *cpreg_values;
    /* Length of the indexes, values, reset_values arrays */
    int32_t cpreg_array_len;
    /* These are used only for migration: incoming data arrives in
     * these fields and is sanity checked in post_load before copying
     * to the working data structures above.
     */
    uint64_t *cpreg_vmstate_indexes;
    uint64_t *cpreg_vmstate_values;
    int32_t cpreg_vmstate_array_len;

    /* Timers used by the generic (architected) timer */
    //QEMUTimer *gt_timer[NUM_GTIMERS];
    /* GPIO outputs for generic timer */
    //qemu_irq gt_timer_outputs[NUM_GTIMERS];

    /* MemoryRegion to use for secure physical accesses */
    MemoryRegion *secure_memory;

    /* 'compatible' string for this CPU for Linux device trees */
    const char *dtb_compatible;

    /* PSCI version for this CPU
     * Bits[31:16] = Major Version
     * Bits[15:0] = Minor Version
     */
    uint32_t psci_version;

    /* Should CPU start in PSCI powered-off state? */
    bool start_powered_off;
    /* CPU currently in PSCI powered-off state */
    bool powered_off;
    /* CPU has virtualization extension */
    bool has_el2;
    /* CPU has security extension */
    bool has_el3;
    /* CPU has PMU (Performance Monitor Unit) */
    bool has_pmu;

    /* CPU has memory protection unit */
    bool has_mpu;
    /* PMSAv7 MPU number of supported regions */
    uint32_t pmsav7_dregion;
    /* v8M SAU number of supported regions */
    uint32_t sau_sregion;

    /* PSCI conduit used to invoke PSCI methods
     * 0 - disabled, 1 - smc, 2 - hvc
     */
    uint32_t psci_conduit;

    /* For v8M, initial value of the Secure VTOR */
    uint32_t init_svtor;

    /* [QEMU_]KVM_ARM_TARGET_* constant for this CPU, or
     * QEMU_KVM_ARM_TARGET_NONE if the kernel doesn't support this CPU type.
     */
    uint32_t kvm_target;

    /* KVM init features for this CPU */
    uint32_t kvm_init_features[7];

    /* Uniprocessor system with MP extensions */
    bool mp_is_up;

    /* Specify the number of cores in this CPU cluster. Used for the L2CTLR
     * register.
     */
    int32_t core_count;

    /* The instance init functions for implementation-specific subclasses
     * set these fields to specify the implementation-dependent values of
     * various constant registers and reset values of non-constant
     * registers.
     * Some of these might become QOM properties eventually.
     * Field names match the official register names as defined in the
     * ARMv7AR ARM Architecture Reference Manual. A reset_ prefix
     * is used for reset values of non-constant registers; no reset_
     * prefix means a constant register.
     */
    uint32_t midr;
    uint32_t revidr;
    uint32_t reset_fpsid;
    uint32_t mvfr0;
    uint32_t mvfr1;
    uint32_t mvfr2;
    uint32_t ctr;
    uint32_t reset_sctlr;
    uint32_t id_pfr0;
    uint32_t id_pfr1;
    uint32_t id_dfr0;
    uint32_t pmceid0;
    uint32_t pmceid1;
    uint32_t id_afr0;
    uint32_t id_mmfr0;
    uint32_t id_mmfr1;
    uint32_t id_mmfr2;
    uint32_t id_mmfr3;
    uint32_t id_mmfr4;
    uint32_t id_isar0;
    uint32_t id_isar1;
    uint32_t id_isar2;
    uint32_t id_isar3;
    uint32_t id_isar4;
    uint32_t id_isar5;
    uint64_t id_aa64pfr0;
    uint64_t id_aa64pfr1;
    uint64_t id_aa64dfr0;
    uint64_t id_aa64dfr1;
    uint64_t id_aa64afr0;
    uint64_t id_aa64afr1;
    uint64_t id_aa64isar0;
    uint64_t id_aa64isar1;
    uint64_t id_aa64mmfr0;
    uint64_t id_aa64mmfr1;
    uint32_t dbgdidr;
    uint32_t clidr;
    uint64_t mp_affinity; /* MP ID without feature bits */
    /* The elements of this array are the CCSIDR values for each cache,
     * in the order L1DCache, L1ICache, L2DCache, L2ICache, etc.
     */
    uint32_t ccsidr[16];
    uint64_t reset_cbar;
    uint32_t reset_auxcr;
    bool reset_hivecs;
    /* DCZ blocksize, in log_2(words), ie low 4 bits of DCZID_EL0 */
    uint32_t dcz_blocksize;
    uint64_t rvbar;

    /* Whether the cfgend input is high (i.e. this CPU should reset into
     * big-endian mode).  This setting isn't used directly: instead it modifies
     * the reset_sctlr value to have SCTLR_B or SCTLR_EE set, depending on the
     * architecture version.
     */
    bool cfgend;

    ARMELChangeHook *el_change_hook;
    void *el_change_hook_opaque;
} ARMCPU;

static inline ARMCPU *arm_env_get_cpu(CPUARMState *env)
{
    return container_of(env, ARMCPU, env);
}

#define ENV_GET_CPU(e) CPU(arm_env_get_cpu(e))

#define ENV_OFFSET offsetof(ARMCPU, env)

#ifndef CONFIG_USER_ONLY
extern const struct VMStateDescription vmstate_arm_cpu;
#endif

void arm_cpu_do_interrupt(CPUState *cpu);
void arm_v7m_cpu_do_interrupt(CPUState *cpu);
bool arm_cpu_exec_interrupt(CPUState *cpu, int int_req);

hwaddr arm_cpu_get_phys_page_attrs_debug(CPUState *cpu, vaddr addr,
                                         MemTxAttrs *attrs);

ARMCPU *cpu_arm_init(struct uc_struct *uc, const char *cpu_model);
int cpu_arm_exec(struct uc_struct *uc, CPUState *cpu);
target_ulong do_arm_semihosting(CPUARMState *env);
void aarch64_sync_32_to_64(CPUARMState *env);
void aarch64_sync_64_to_32(CPUARMState *env);

static inline bool is_a64(CPUARMState *env)
{
    return env->aarch64;
}

/* you can call this signal handler from your SIGBUS and SIGSEGV
   signal handlers to inform the virtual CPU of exceptions. non zero
   is returned if the signal was handled by the virtual CPU.  */
int cpu_arm_signal_handler(int host_signum, void *pinfo,
                           void *puc);

/**
 * pmccntr_sync
 * @env: CPUARMState
 *
 * Synchronises the counter in the PMCCNTR. This must always be called twice,
 * once before any action that might affect the timer and again afterwards.
 * The function is used to swap the state of the register if required.
 * This only happens when not in user mode (!CONFIG_USER_ONLY)
 */
void pmccntr_sync(CPUARMState *env);

/* SCTLR bit meanings. Several bits have been reused in newer
 * versions of the architecture; in that case we define constants
 * for both old and new bit meanings. Code which tests against those
 * bits should probably check or otherwise arrange that the CPU
 * is the architectural version it expects.
 */
#define SCTLR_M       (1U << 0)
#define SCTLR_A       (1U << 1)
#define SCTLR_C       (1U << 2)
#define SCTLR_W       (1U << 3) /* up to v6; RAO in v7 */
#define SCTLR_SA      (1U << 3)
#define SCTLR_P       (1U << 4) /* up to v5; RAO in v6 and v7 */
#define SCTLR_SA0     (1U << 4) /* v8 onward, AArch64 only */
#define SCTLR_D       (1U << 5) /* up to v5; RAO in v6 */
#define SCTLR_CP15BEN (1U << 5) /* v7 onward */
#define SCTLR_L       (1U << 6) /* up to v5; RAO in v6 and v7; RAZ in v8 */
#define SCTLR_B       (1U << 7) /* up to v6; RAZ in v7 */
#define SCTLR_ITD     (1U << 7) /* v8 onward */
#define SCTLR_S       (1U << 8) /* up to v6; RAZ in v7 */
#define SCTLR_SED     (1U << 8) /* v8 onward */
#define SCTLR_R       (1U << 9) /* up to v6; RAZ in v7 */
#define SCTLR_UMA     (1U << 9) /* v8 onward, AArch64 only */
#define SCTLR_F       (1U << 10) /* up to v6 */
#define SCTLR_SW      (1U << 10) /* v7 onward */
#define SCTLR_Z       (1U << 11)
#define SCTLR_I       (1U << 12)
#define SCTLR_V       (1U << 13)
#define SCTLR_RR      (1U << 14) /* up to v7 */
#define SCTLR_DZE     (1U << 14) /* v8 onward, AArch64 only */
#define SCTLR_L4      (1U << 15) /* up to v6; RAZ in v7 */
#define SCTLR_UCT     (1U << 15) /* v8 onward, AArch64 only */
#define SCTLR_DT      (1U << 16) /* up to ??, RAO in v6 and v7 */
#define SCTLR_nTWI    (1U << 16) /* v8 onward */
#define SCTLR_HA      (1U << 17)
#define SCTLR_BR      (1U << 17) /* PMSA only */
#define SCTLR_IT      (1U << 18) /* up to ??, RAO in v6 and v7 */
#define SCTLR_nTWE    (1U << 18) /* v8 onward */
#define SCTLR_WXN     (1U << 19)
#define SCTLR_ST      (1U << 20) /* up to ??, RAZ in v6 */
#define SCTLR_UWXN    (1U << 20) /* v7 onward */
#define SCTLR_FI      (1U << 21)
#define SCTLR_U       (1U << 22)
#define SCTLR_XP      (1U << 23) /* up to v6; v7 onward RAO */
#define SCTLR_VE      (1U << 24) /* up to v7 */
#define SCTLR_E0E     (1U << 24) /* v8 onward, AArch64 only */
#define SCTLR_EE      (1U << 25)
#define SCTLR_L2      (1U << 26) /* up to v6, RAZ in v7 */
#define SCTLR_UCI     (1U << 26) /* v8 onward, AArch64 only */
#define SCTLR_NMFI    (1U << 27)
#define SCTLR_TRE     (1U << 28)
#define SCTLR_AFE     (1U << 29)
#define SCTLR_TE      (1U << 30)

#define CPTR_TCPAC    (1U << 31)
#define CPTR_TTA      (1U << 20)
#define CPTR_TFP      (1U << 10)
#define CPTR_TZ       (1U << 8)   /* CPTR_EL2 */
#define CPTR_EZ       (1U << 8)   /* CPTR_EL3 */

#define MDCR_EPMAD    (1U << 21)
#define MDCR_EDAD     (1U << 20)
#define MDCR_SPME     (1U << 17)
#define MDCR_SDD      (1U << 16)
#define MDCR_SPD      (3U << 14)
#define MDCR_TDRA     (1U << 11)
#define MDCR_TDOSA    (1U << 10)
#define MDCR_TDA      (1U << 9)
#define MDCR_TDE      (1U << 8)
#define MDCR_HPME     (1U << 7)
#define MDCR_TPM      (1U << 6)
#define MDCR_TPMCR    (1U << 5)

/* Not all of the MDCR_EL3 bits are present in the 32-bit SDCR */
#define SDCR_VALID_MASK (MDCR_EPMAD | MDCR_EDAD | MDCR_SPME | MDCR_SPD)

#define CPSR_M (0x1fU)
#define CPSR_T (1U << 5)
#define CPSR_F (1U << 6)
#define CPSR_I (1U << 7)
#define CPSR_A (1U << 8)
#define CPSR_E (1U << 9)
#define CPSR_IT_2_7 (0xfc00U)
#define CPSR_GE (0xfU << 16)
#define CPSR_IL (1U << 20)
/* Note that the RESERVED bits include bit 21, which is PSTATE_SS in
 * an AArch64 SPSR but RES0 in AArch32 SPSR and CPSR. In QEMU we use
 * env->uncached_cpsr bit 21 to store PSTATE.SS when executing in AArch32,
 * where it is live state but not accessible to the AArch32 code.
 */
#define CPSR_RESERVED (0x7U << 21)
#define CPSR_J (1U << 24)
#define CPSR_IT_0_1 (3U << 25)
#define CPSR_Q (1U << 27)
#define CPSR_V (1U << 28)
#define CPSR_C (1U << 29)
#define CPSR_Z (1U << 30)
#define CPSR_N (1U << 31)
#define CPSR_NZCV (CPSR_N | CPSR_Z | CPSR_C | CPSR_V)
#define CPSR_AIF (CPSR_A | CPSR_I | CPSR_F)

#define CPSR_IT (CPSR_IT_0_1 | CPSR_IT_2_7)
#define CACHED_CPSR_BITS (CPSR_T | CPSR_AIF | CPSR_GE | CPSR_IT | CPSR_Q \
    | CPSR_NZCV)
/* Bits writable in user mode.  */
#define CPSR_USER (CPSR_NZCV | CPSR_Q | CPSR_GE)
/* Execution state bits.  MRS read as zero, MSR writes ignored.  */
#define CPSR_EXEC (CPSR_T | CPSR_IT | CPSR_J | CPSR_IL)
/* Mask of bits which may be set by exception return copying them from SPSR */
#define CPSR_ERET_MASK (~CPSR_RESERVED)

/* Bit definitions for M profile XPSR. Most are the same as CPSR. */
#define XPSR_EXCP 0x1ffU
#define XPSR_SPREALIGN (1U << 9) /* Only set in exception stack frames */
#define XPSR_IT_2_7 CPSR_IT_2_7
#define XPSR_GE CPSR_GE
#define XPSR_SFPA (1U << 20) /* Only set in exception stack frames */
#define XPSR_T (1U << 24) /* Not the same as CPSR_T ! */
#define XPSR_IT_0_1 CPSR_IT_0_1
#define XPSR_Q CPSR_Q
#define XPSR_V CPSR_V
#define XPSR_C CPSR_C
#define XPSR_Z CPSR_Z
#define XPSR_N CPSR_N
#define XPSR_NZCV CPSR_NZCV
#define XPSR_IT CPSR_IT

#define TTBCR_N      (7U << 0) /* TTBCR.EAE==0 */
#define TTBCR_T0SZ   (7U << 0) /* TTBCR.EAE==1 */
#define TTBCR_PD0    (1U << 4)
#define TTBCR_PD1    (1U << 5)
#define TTBCR_EPD0   (1U << 7)
#define TTBCR_IRGN0  (3U << 8)
#define TTBCR_ORGN0  (3U << 10)
#define TTBCR_SH0    (3U << 12)
#define TTBCR_T1SZ   (3U << 16)
#define TTBCR_A1     (1U << 22)
#define TTBCR_EPD1   (1U << 23)
#define TTBCR_IRGN1  (3U << 24)
#define TTBCR_ORGN1  (3U << 26)
#define TTBCR_SH1    (1U << 28)
#define TTBCR_EAE    (1U << 31)

/* Bit definitions for ARMv8 SPSR (PSTATE) format.
 * Only these are valid when in AArch64 mode; in
 * AArch32 mode SPSRs are basically CPSR-format.
 */
#define PSTATE_SP (1U)
#define PSTATE_M (0xFU)
#define PSTATE_nRW (1U << 4)
#define PSTATE_F (1U << 6)
#define PSTATE_I (1U << 7)
#define PSTATE_A (1U << 8)
#define PSTATE_D (1U << 9)
#define PSTATE_IL (1U << 20)
#define PSTATE_SS (1U << 21)
#define PSTATE_V (1U << 28)
#define PSTATE_C (1U << 29)
#define PSTATE_Z (1U << 30)
#define PSTATE_N (1U << 31)
#define PSTATE_NZCV (PSTATE_N | PSTATE_Z | PSTATE_C | PSTATE_V)
#define PSTATE_DAIF (PSTATE_D | PSTATE_A | PSTATE_I | PSTATE_F)
#define CACHED_PSTATE_BITS (PSTATE_NZCV | PSTATE_DAIF)
/* Mode values for AArch64 */
#define PSTATE_MODE_EL3h 13
#define PSTATE_MODE_EL3t 12
#define PSTATE_MODE_EL2h 9
#define PSTATE_MODE_EL2t 8
#define PSTATE_MODE_EL1h 5
#define PSTATE_MODE_EL1t 4
#define PSTATE_MODE_EL0t 0

/* Write a new value to v7m.exception, thus transitioning into or out
 * of Handler mode; this may result in a change of active stack pointer.
 */
void write_v7m_exception(CPUARMState *env, uint32_t new_exc);

/* Map EL and handler into a PSTATE_MODE.  */
static inline unsigned int aarch64_pstate_mode(unsigned int el, bool handler)
{
    return (el << 2) | handler;
}

/* Return the current PSTATE value. For the moment we don't support 32<->64 bit
 * interprocessing, so we don't attempt to sync with the cpsr state used by
 * the 32 bit decoder.
 */
static inline uint32_t pstate_read(CPUARMState *env)
{
    int ZF;

    ZF = (env->ZF == 0);
    return (env->NF & 0x80000000) | (ZF << 30)
        | (env->CF << 29) | ((env->VF & 0x80000000) >> 3)
        | env->pstate | env->daif;
}

static inline void pstate_write(CPUARMState *env, uint32_t val)
{
    env->ZF = (~val) & PSTATE_Z;
    env->NF = val;
    env->CF = (val >> 29) & 1;
    env->VF = (val << 3) & 0x80000000;
    env->daif = val & PSTATE_DAIF;
    env->pstate = val & ~CACHED_PSTATE_BITS;
}

/* Return the current CPSR value.  */
uint32_t cpsr_read(CPUARMState *env);

typedef enum CPSRWriteType {
    CPSRWriteByInstr = 0,         /* from guest MSR or CPS */
    CPSRWriteExceptionReturn = 1, /* from guest exception return insn */
    CPSRWriteRaw = 2,             /* trust values, do not switch reg banks */
    CPSRWriteByGDBStub = 3,       /* from the GDB stub */
} CPSRWriteType;

/* Set the CPSR.  Note that some bits of mask must be all-set or all-clear.*/
void cpsr_write(CPUARMState *env, uint32_t val, uint32_t mask,
                CPSRWriteType write_type);

/* Return the current xPSR value.  */
static inline uint32_t xpsr_read(CPUARMState *env)
{
    int ZF;
    ZF = (env->ZF == 0);
    return (env->NF & 0x80000000) | (ZF << 30)
        | (env->CF << 29) | ((env->VF & 0x80000000) >> 3) | (env->QF << 27)
        | (env->thumb << 24) | ((env->condexec_bits & 3) << 25)
        | ((env->condexec_bits & 0xfc) << 8)
        | env->v7m.exception;
}

/* Set the xPSR.  Note that some bits of mask must be all-set or all-clear.  */
static inline void xpsr_write(CPUARMState *env, uint32_t val, uint32_t mask)
{
    if (mask & XPSR_NZCV) {
        env->ZF = (~val) & XPSR_Z;
        env->NF = val;
        env->CF = (val >> 29) & 1;
        env->VF = (val << 3) & 0x80000000;
    }
    if (mask & XPSR_Q) {
        env->QF = ((val & XPSR_Q) != 0);
    }
    if (mask & XPSR_T) {
        env->thumb = ((val & XPSR_T) != 0);
    }
    if (mask & XPSR_IT_0_1) {
        env->condexec_bits &= ~3;
        env->condexec_bits |= (val >> 25) & 3;
    }
    if (mask & XPSR_IT_2_7) {
        env->condexec_bits &= 3;
        env->condexec_bits |= (val >> 8) & 0xfc;
    }
    if (mask & XPSR_EXCP) {
        /* Note that this only happens on exception exit */
        write_v7m_exception(env, val & XPSR_EXCP);
    }
}

#define HCR_VM        (1ULL << 0)
#define HCR_SWIO      (1ULL << 1)
#define HCR_PTW       (1ULL << 2)
#define HCR_FMO       (1ULL << 3)
#define HCR_IMO       (1ULL << 4)
#define HCR_AMO       (1ULL << 5)
#define HCR_VF        (1ULL << 6)
#define HCR_VI        (1ULL << 7)
#define HCR_VSE       (1ULL << 8)
#define HCR_FB        (1ULL << 9)
#define HCR_BSU_MASK  (3ULL << 10)
#define HCR_DC        (1ULL << 12)
#define HCR_TWI       (1ULL << 13)
#define HCR_TWE       (1ULL << 14)
#define HCR_TID0      (1ULL << 15)
#define HCR_TID1      (1ULL << 16)
#define HCR_TID2      (1ULL << 17)
#define HCR_TID3      (1ULL << 18)
#define HCR_TSC       (1ULL << 19)
#define HCR_TIDCP     (1ULL << 20)
#define HCR_TACR      (1ULL << 21)
#define HCR_TSW       (1ULL << 22)
#define HCR_TPC       (1ULL << 23)
#define HCR_TPU       (1ULL << 24)
#define HCR_TTLB      (1ULL << 25)
#define HCR_TVM       (1ULL << 26)
#define HCR_TGE       (1ULL << 27)
#define HCR_TDZ       (1ULL << 28)
#define HCR_HCD       (1ULL << 29)
#define HCR_TRVM      (1ULL << 30)
#define HCR_RW        (1ULL << 31)
#define HCR_CD        (1ULL << 32)
#define HCR_ID        (1ULL << 33)
#define HCR_MASK      ((1ULL << 34) - 1)

#define SCR_NS                (1U << 0)
#define SCR_IRQ               (1U << 1)
#define SCR_FIQ               (1U << 2)
#define SCR_EA                (1U << 3)
#define SCR_FW                (1U << 4)
#define SCR_AW                (1U << 5)
#define SCR_NET               (1U << 6)
#define SCR_SMD               (1U << 7)
#define SCR_HCE               (1U << 8)
#define SCR_SIF               (1U << 9)
#define SCR_RW                (1U << 10)
#define SCR_ST                (1U << 11)
#define SCR_TWI               (1U << 12)
#define SCR_TWE               (1U << 13)
#define SCR_AARCH32_MASK      (0x3fff & ~(SCR_RW | SCR_ST))
#define SCR_AARCH64_MASK      (0x3fff & ~SCR_NET)

/* Return the current FPSCR value.  */
uint32_t vfp_get_fpscr(CPUARMState *env);
void vfp_set_fpscr(CPUARMState *env, uint32_t val);

/* FPCR, Floating Point Control Register
 * FPSR, Floating Poiht Status Register
 *
 * For A64 the FPSCR is split into two logically distinct registers,
 * FPCR and FPSR. However since they still use non-overlapping bits
 * we store the underlying state in fpscr and just mask on read/write.
 */
#define FPSR_MASK 0xf800009f
#define FPCR_MASK 0x07f79f00

#define FPCR_FZ16   (1 << 19)   /* ARMv8.2+, FP16 flush-to-zero */
#define FPCR_FZ     (1 << 24)   /* Flush-to-zero enable bit */
#define FPCR_DN     (1 << 25)   /* Default NaN enable bit */

static inline uint32_t vfp_get_fpsr(CPUARMState *env)
{
    return vfp_get_fpscr(env) & FPSR_MASK;
}

static inline void vfp_set_fpsr(CPUARMState *env, uint32_t val)
{
    uint32_t new_fpscr = (vfp_get_fpscr(env) & ~FPSR_MASK) | (val & FPSR_MASK);
    vfp_set_fpscr(env, new_fpscr);
}

static inline uint32_t vfp_get_fpcr(CPUARMState *env)
{
    return vfp_get_fpscr(env) & FPCR_MASK;
}

static inline void vfp_set_fpcr(CPUARMState *env, uint32_t val)
{
    uint32_t new_fpscr = (vfp_get_fpscr(env) & ~FPCR_MASK) | (val & FPCR_MASK);
    vfp_set_fpscr(env, new_fpscr);
}

enum arm_cpu_mode {
  ARM_CPU_MODE_USR = 0x10,
  ARM_CPU_MODE_FIQ = 0x11,
  ARM_CPU_MODE_IRQ = 0x12,
  ARM_CPU_MODE_SVC = 0x13,
  ARM_CPU_MODE_MON = 0x16,
  ARM_CPU_MODE_ABT = 0x17,
  ARM_CPU_MODE_HYP = 0x1a,
  ARM_CPU_MODE_UND = 0x1b,
  ARM_CPU_MODE_SYS = 0x1f
};

/* VFP system registers.  */
#define ARM_VFP_FPSID   0
#define ARM_VFP_FPSCR   1
#define ARM_VFP_MVFR2   5
#define ARM_VFP_MVFR1   6
#define ARM_VFP_MVFR0   7
#define ARM_VFP_FPEXC   8
#define ARM_VFP_FPINST  9
#define ARM_VFP_FPINST2 10

/* iwMMXt coprocessor control registers.  */
#define ARM_IWMMXT_wCID		0
#define ARM_IWMMXT_wCon		1
#define ARM_IWMMXT_wCSSF	2
#define ARM_IWMMXT_wCASF	3
#define ARM_IWMMXT_wCGR0	8
#define ARM_IWMMXT_wCGR1	9
#define ARM_IWMMXT_wCGR2	10
#define ARM_IWMMXT_wCGR3	11

/* V7M CCR bits */
FIELD(V7M_CCR, NONBASETHRDENA, 0, 1)
FIELD(V7M_CCR, USERSETMPEND, 1, 1)
FIELD(V7M_CCR, UNALIGN_TRP, 3, 1)
FIELD(V7M_CCR, DIV_0_TRP, 4, 1)
FIELD(V7M_CCR, BFHFNMIGN, 8, 1)
FIELD(V7M_CCR, STKALIGN, 9, 1)
FIELD(V7M_CCR, DC, 16, 1)
FIELD(V7M_CCR, IC, 17, 1)

/* V7M SCR bits */
FIELD(V7M_SCR, SLEEPONEXIT, 1, 1)
FIELD(V7M_SCR, SLEEPDEEP, 2, 1)
FIELD(V7M_SCR, SLEEPDEEPS, 3, 1)
FIELD(V7M_SCR, SEVONPEND, 4, 1)

/* V7M AIRCR bits */
FIELD(V7M_AIRCR, VECTRESET, 0, 1)
FIELD(V7M_AIRCR, VECTCLRACTIVE, 1, 1)
FIELD(V7M_AIRCR, SYSRESETREQ, 2, 1)
FIELD(V7M_AIRCR, SYSRESETREQS, 3, 1)
FIELD(V7M_AIRCR, PRIGROUP, 8, 3)
FIELD(V7M_AIRCR, BFHFNMINS, 13, 1)
FIELD(V7M_AIRCR, PRIS, 14, 1)
FIELD(V7M_AIRCR, ENDIANNESS, 15, 1)
FIELD(V7M_AIRCR, VECTKEY, 16, 16)

/* V7M CFSR bits for MMFSR */
FIELD(V7M_CFSR, IACCVIOL, 0, 1)
FIELD(V7M_CFSR, DACCVIOL, 1, 1)
FIELD(V7M_CFSR, MUNSTKERR, 3, 1)
FIELD(V7M_CFSR, MSTKERR, 4, 1)
FIELD(V7M_CFSR, MLSPERR, 5, 1)
FIELD(V7M_CFSR, MMARVALID, 7, 1)

/* V7M CFSR bits for BFSR */
FIELD(V7M_CFSR, IBUSERR, 8 + 0, 1)
FIELD(V7M_CFSR, PRECISERR, 8 + 1, 1)
FIELD(V7M_CFSR, IMPRECISERR, 8 + 2, 1)
FIELD(V7M_CFSR, UNSTKERR, 8 + 3, 1)
FIELD(V7M_CFSR, STKERR, 8 + 4, 1)
FIELD(V7M_CFSR, LSPERR, 8 + 5, 1)
FIELD(V7M_CFSR, BFARVALID, 8 + 7, 1)

/* V7M CFSR bits for UFSR */
FIELD(V7M_CFSR, UNDEFINSTR, 16 + 0, 1)
FIELD(V7M_CFSR, INVSTATE, 16 + 1, 1)
FIELD(V7M_CFSR, INVPC, 16 + 2, 1)
FIELD(V7M_CFSR, NOCP, 16 + 3, 1)
FIELD(V7M_CFSR, UNALIGNED, 16 + 8, 1)
FIELD(V7M_CFSR, DIVBYZERO, 16 + 9, 1)

/* V7M CFSR bit masks covering all of the subregister bits */
FIELD(V7M_CFSR, MMFSR, 0, 8)
FIELD(V7M_CFSR, BFSR, 8, 8)
FIELD(V7M_CFSR, UFSR, 16, 16)

/* V7M HFSR bits */
FIELD(V7M_HFSR, VECTTBL, 1, 1)
FIELD(V7M_HFSR, FORCED, 30, 1)
FIELD(V7M_HFSR, DEBUGEVT, 31, 1)

/* V7M DFSR bits */
FIELD(V7M_DFSR, HALTED, 0, 1)
FIELD(V7M_DFSR, BKPT, 1, 1)
FIELD(V7M_DFSR, DWTTRAP, 2, 1)
FIELD(V7M_DFSR, VCATCH, 3, 1)
FIELD(V7M_DFSR, EXTERNAL, 4, 1)

/* V7M SFSR bits */
FIELD(V7M_SFSR, INVEP, 0, 1)
FIELD(V7M_SFSR, INVIS, 1, 1)
FIELD(V7M_SFSR, INVER, 2, 1)
FIELD(V7M_SFSR, AUVIOL, 3, 1)
FIELD(V7M_SFSR, INVTRAN, 4, 1)
FIELD(V7M_SFSR, LSPERR, 5, 1)
FIELD(V7M_SFSR, SFARVALID, 6, 1)
FIELD(V7M_SFSR, LSERR, 7, 1)

/* v7M MPU_CTRL bits */
FIELD(V7M_MPU_CTRL, ENABLE, 0, 1)
FIELD(V7M_MPU_CTRL, HFNMIENA, 1, 1)
FIELD(V7M_MPU_CTRL, PRIVDEFENA, 2, 1)

/* v7M CLIDR bits */
FIELD(V7M_CLIDR, CTYPE_ALL, 0, 21)
FIELD(V7M_CLIDR, LOUIS, 21, 3)
FIELD(V7M_CLIDR, LOC, 24, 3)
FIELD(V7M_CLIDR, LOUU, 27, 3)
FIELD(V7M_CLIDR, ICB, 30, 2)

FIELD(V7M_CSSELR, IND, 0, 1)
FIELD(V7M_CSSELR, LEVEL, 1, 3)
/* We use the combination of InD and Level to index into cpu->ccsidr[];
 * define a mask for this and check that it doesn't permit running off
 * the end of the array.
 */
FIELD(V7M_CSSELR, INDEX, 0, 4)

QEMU_BUILD_BUG_ON(ARRAY_SIZE(((ARMCPU *)0)->ccsidr) <= R_V7M_CSSELR_INDEX_MASK);

/* If adding a feature bit which corresponds to a Linux ELF
 * HWCAP bit, remember to update the feature-bit-to-hwcap
 * mapping in linux-user/elfload.c:get_elf_hwcap().
 */
enum arm_features {
    ARM_FEATURE_VFP,
    ARM_FEATURE_AUXCR,  /* ARM1026 Auxiliary control register.  */
    ARM_FEATURE_XSCALE, /* Intel XScale extensions.  */
    ARM_FEATURE_IWMMXT, /* Intel iwMMXt extension.  */
    ARM_FEATURE_V6,
    ARM_FEATURE_V6K,
    ARM_FEATURE_V7,
    ARM_FEATURE_THUMB2,
    ARM_FEATURE_PMSA,   /* no MMU; may have Memory Protection Unit */
    ARM_FEATURE_VFP3,
    ARM_FEATURE_VFP_FP16,
    ARM_FEATURE_NEON,
    ARM_FEATURE_THUMB_DIV, /* divide supported in Thumb encoding */
    ARM_FEATURE_M, /* Microcontroller profile.  */
    ARM_FEATURE_OMAPCP, /* OMAP specific CP15 ops handling.  */
    ARM_FEATURE_THUMB2EE,
    ARM_FEATURE_V7MP,    /* v7 Multiprocessing Extensions */
    ARM_FEATURE_V4T,
    ARM_FEATURE_V5,
    ARM_FEATURE_STRONGARM,
    ARM_FEATURE_VAPA, /* cp15 VA to PA lookups */
    ARM_FEATURE_ARM_DIV, /* divide supported in ARM encoding */
    ARM_FEATURE_VFP4, /* VFPv4 (implies that NEON is v2) */
    ARM_FEATURE_GENERIC_TIMER,
    ARM_FEATURE_MVFR, /* Media and VFP Feature Registers 0 and 1 */
    ARM_FEATURE_DUMMY_C15_REGS, /* RAZ/WI all of cp15 crn=15 */
    ARM_FEATURE_CACHE_TEST_CLEAN, /* 926/1026 style test-and-clean ops */
    ARM_FEATURE_CACHE_DIRTY_REG, /* 1136/1176 cache dirty status register */
    ARM_FEATURE_CACHE_BLOCK_OPS, /* v6 optional cache block operations */
    ARM_FEATURE_MPIDR, /* has cp15 MPIDR */
    ARM_FEATURE_PXN, /* has Privileged Execute Never bit */
    ARM_FEATURE_LPAE, /* has Large Physical Address Extension */
    ARM_FEATURE_V8,
    ARM_FEATURE_AARCH64, /* supports 64 bit mode */
    ARM_FEATURE_V8_AES, /* implements AES part of v8 Crypto Extensions */
    ARM_FEATURE_CBAR, /* has cp15 CBAR */
    ARM_FEATURE_CRC, /* ARMv8 CRC instructions */
    ARM_FEATURE_CBAR_RO, /* has cp15 CBAR and it is read-only */
    ARM_FEATURE_EL2, /* has EL2 Virtualization support */
    ARM_FEATURE_EL3, /* has EL3 Secure monitor support */
    ARM_FEATURE_V8_SHA1, /* implements SHA1 part of v8 Crypto Extensions */
    ARM_FEATURE_V8_SHA256, /* implements SHA256 part of v8 Crypto Extensions */
    ARM_FEATURE_V8_PMULL, /* implements PMULL part of v8 Crypto Extensions */
    ARM_FEATURE_THUMB_DSP, /* DSP insns supported in the Thumb encodings */
    ARM_FEATURE_PMU, /* has PMU support */
    ARM_FEATURE_VBAR, /* has cp15 VBAR */
    ARM_FEATURE_M_SECURITY, /* M profile Security Extension */
    ARM_FEATURE_JAZELLE, /* has (trivial) Jazelle implementation */
    ARM_FEATURE_SVE, /* has Scalable Vector Extension */
    ARM_FEATURE_V8_SHA512, /* implements SHA512 part of v8 Crypto Extensions */
    ARM_FEATURE_V8_SHA3, /* implements SHA3 part of v8 Crypto Extensions */
    ARM_FEATURE_V8_SM3, /* implements SM3 part of v8 Crypto Extensions */
    ARM_FEATURE_V8_SM4, /* implements SM4 part of v8 Crypto Extensions */
    ARM_FEATURE_V8_RDM, /* implements v8.1 simd round multiply */
    ARM_FEATURE_V8_FP16, /* implements v8.2 half-precision float */
    ARM_FEATURE_V8_FCMA, /* has complex number part of v8.3 extensions.  */
};

static inline int arm_feature(CPUARMState *env, int feature)
{
    return (env->features & (1ULL << feature)) != 0;
}

#if !defined(CONFIG_USER_ONLY)
/* Return true if exception levels below EL3 are in secure state,
 * or would be following an exception return to that level.
 * Unlike arm_is_secure() (which is always a question about the
 * _current_ state of the CPU) this doesn't care about the current
 * EL or mode.
 */
static inline bool arm_is_secure_below_el3(CPUARMState *env)
{
    if (arm_feature(env, ARM_FEATURE_EL3)) {
        return !(env->cp15.scr_el3 & SCR_NS);
    } else {
        /* If EL3 is not supported then the secure state is implementation
         * defined, in which case QEMU defaults to non-secure.
         */
        return false;
    }
}

/* Return true if the CPU is AArch64 EL3 or AArch32 Mon */
static inline bool arm_is_el3_or_mon(CPUARMState *env)
{
    if (arm_feature(env, ARM_FEATURE_EL3)) {
        if (is_a64(env) && extract32(env->pstate, 2, 2) == 3) {
            /* CPU currently in AArch64 state and EL3 */
            return true;
        } else if (!is_a64(env) &&
                (env->uncached_cpsr & CPSR_M) == ARM_CPU_MODE_MON) {
            /* CPU currently in AArch32 state and monitor mode */
            return true;
        }
    }
    return false;
}

/* Return true if the processor is in secure state */
static inline bool arm_is_secure(CPUARMState *env)
{
    if (arm_is_el3_or_mon(env)) {
        return true;
    }
    return arm_is_secure_below_el3(env);
}

#else
static inline bool arm_is_secure_below_el3(CPUARMState *env)
{
    return false;
}

static inline bool arm_is_secure(CPUARMState *env)
{
    return false;
}
#endif

/* Return true if the specified exception level is running in AArch64 state. */
static inline bool arm_el_is_aa64(CPUARMState *env, int el)
{
    /* This isn't valid for EL0 (if we're in EL0, is_a64() is what you want,
     * and if we're not in EL0 then the state of EL0 isn't well defined.)
     */
    assert(el >= 1 && el <= 3);
    bool aa64 = arm_feature(env, ARM_FEATURE_AARCH64);

    /* The highest exception level is always at the maximum supported
     * register width, and then lower levels have a register width controlled
     * by bits in the SCR or HCR registers.
     */
    if (el == 3) {
        return aa64;
    }

    if (arm_feature(env, ARM_FEATURE_EL3)) {
        aa64 = aa64 && (env->cp15.scr_el3 & SCR_RW);
    }

    if (el == 2) {
        return aa64;
    }

    if (arm_feature(env, ARM_FEATURE_EL2) && !arm_is_secure_below_el3(env)) {
        aa64 = aa64 && (env->cp15.hcr_el2 & HCR_RW);
    }

    return aa64;
}

/* Function for determing whether guest cp register reads and writes should
 * access the secure or non-secure bank of a cp register.  When EL3 is
 * operating in AArch32 state, the NS-bit determines whether the secure
 * instance of a cp register should be used. When EL3 is AArch64 (or if
 * it doesn't exist at all) then there is no register banking, and all
 * accesses are to the non-secure version.
 */
static inline bool access_secure_reg(CPUARMState *env)
{
    bool ret = (arm_feature(env, ARM_FEATURE_EL3) &&
                !arm_el_is_aa64(env, 3) &&
                !(env->cp15.scr_el3 & SCR_NS));

    return ret;
}

/* Macros for accessing a specified CP register bank */
#define A32_BANKED_REG_GET(_env, _regname, _secure)    \
    ((_secure) ? (_env)->cp15._regname##_s : (_env)->cp15._regname##_ns)

#define A32_BANKED_REG_SET(_env, _regname, _secure, _val)   \
    do {                                                \
        if (_secure) {                                   \
            (_env)->cp15._regname##_s = (_val);            \
        } else {                                        \
            (_env)->cp15._regname##_ns = (_val);           \
        }                                               \
    } while (0)

/* Macros for automatically accessing a specific CP register bank depending on
 * the current secure state of the system.  These macros are not intended for
 * supporting instruction translation reads/writes as these are dependent
 * solely on the SCR.NS bit and not the mode.
 */
#define A32_BANKED_CURRENT_REG_GET(_env, _regname)        \
    A32_BANKED_REG_GET((_env), _regname,                \
                       (arm_is_secure(_env) && !arm_el_is_aa64((_env), 3)))

#define A32_BANKED_CURRENT_REG_SET(_env, _regname, _val)                       \
    A32_BANKED_REG_SET((_env), _regname,                                    \
                       (arm_is_secure(_env) && !arm_el_is_aa64((_env), 3)), \
                       (_val))

void arm_cpu_list(FILE *f, fprintf_function cpu_fprintf);
uint32_t arm_phys_excp_target_el(CPUState *cs, uint32_t excp_idx,
                                 uint32_t cur_el, bool secure);

/* Interface between CPU and Interrupt controller.  */
#ifndef CONFIG_USER_ONLY
bool armv7m_nvic_can_take_pending_exception(void *opaque);
#else
static inline bool armv7m_nvic_can_take_pending_exception(void *opaque)
{
    return true;
}
#endif
/**
 * armv7m_nvic_set_pending: mark the specified exception as pending
 * @opaque: the NVIC
 * @irq: the exception number to mark pending
 * @secure: false for non-banked exceptions or for the nonsecure
 * version of a banked exception, true for the secure version of a banked
 * exception.
 *
 * Marks the specified exception as pending. Note that we will assert()
 * if @secure is true and @irq does not specify one of the fixed set
 * of architecturally banked exceptions.
 */
void armv7m_nvic_set_pending(void *opaque, int irq, bool secure);
/**
 * armv7m_nvic_set_pending_derived: mark this derived exception as pending
 * @opaque: the NVIC
 * @irq: the exception number to mark pending
 * @secure: false for non-banked exceptions or for the nonsecure
 * version of a banked exception, true for the secure version of a banked
 * exception.
 *
 * Similar to armv7m_nvic_set_pending(), but specifically for derived
 * exceptions (exceptions generated in the course of trying to take
 * a different exception).
 */
void armv7m_nvic_set_pending_derived(void *opaque, int irq, bool secure);
/**
 * armv7m_nvic_get_pending_irq_info: return highest priority pending
 *    exception, and whether it targets Secure state
 * @opaque: the NVIC
 * @pirq: set to pending exception number
 * @ptargets_secure: set to whether pending exception targets Secure
 *
 * This function writes the number of the highest priority pending
 * exception (the one which would be made active by
 * armv7m_nvic_acknowledge_irq()) to @pirq, and sets @ptargets_secure
 * to true if the current highest priority pending exception should
 * be taken to Secure state, false for NS.
 */
void armv7m_nvic_get_pending_irq_info(void *opaque, int *pirq,
                                      bool *ptargets_secure);
/**
 * armv7m_nvic_acknowledge_irq: make highest priority pending exception active
 * @opaque: the NVIC
 *
 * Move the current highest priority pending exception from the pending
 * state to the active state, and update v7m.exception to indicate that
 * it is the exception currently being handled.
 */
void armv7m_nvic_acknowledge_irq(void *opaque);
/**
 * armv7m_nvic_complete_irq: complete specified interrupt or exception
 * @opaque: the NVIC
 * @irq: the exception number to complete
 * @secure: true if this exception was secure
 *
 * Returns: -1 if the irq was not active
 *           1 if completing this irq brought us back to base (no active irqs)
 *           0 if there is still an irq active after this one was completed
 * (Ignoring -1, this is the same as the RETTOBASE value before completion.)
 */
int armv7m_nvic_complete_irq(void *opaque, int irq, bool secure);
/**
 * armv7m_nvic_raw_execution_priority: return the raw execution priority
 * @opaque: the NVIC
 *
 * Returns: the raw execution priority as defined by the v8M architecture.
 * This is the execution priority minus the effects of AIRCR.PRIS,
 * and minus any PRIMASK/FAULTMASK/BASEPRI priority boosting.
 * (v8M ARM ARM I_PKLD.)
 */
int armv7m_nvic_raw_execution_priority(void *opaque);
/**
 * armv7m_nvic_neg_prio_requested: return true if the requested execution
 * priority is negative for the specified security state.
 * @opaque: the NVIC
 * @secure: the security state to test
 * This corresponds to the pseudocode IsReqExecPriNeg().
 */
#ifndef CONFIG_USER_ONLY
bool armv7m_nvic_neg_prio_requested(void *opaque, bool secure);
#else
static inline bool armv7m_nvic_neg_prio_requested(void *opaque, bool secure)
{
    return false;
}
#endif

/* Interface for defining coprocessor registers.
 * Registers are defined in tables of arm_cp_reginfo structs
 * which are passed to define_arm_cp_regs().
 */

/* When looking up a coprocessor register we look for it
 * via an integer which encodes all of:
 *  coprocessor number
 *  Crn, Crm, opc1, opc2 fields
 *  32 or 64 bit register (ie is it accessed via MRC/MCR
 *    or via MRRC/MCRR?)
 *  non-secure/secure bank (AArch32 only)
 * We allow 4 bits for opc1 because MRRC/MCRR have a 4 bit field.
 * (In this case crn and opc2 should be zero.)
 * For AArch64, there is no 32/64 bit size distinction;
 * instead all registers have a 2 bit op0, 3 bit op1 and op2,
 * and 4 bit CRn and CRm. The encoding patterns are chosen
 * to be easy to convert to and from the KVM encodings, and also
 * so that the hashtable can contain both AArch32 and AArch64
 * registers (to allow for interprocessing where we might run
 * 32 bit code on a 64 bit core).
 */
/* This bit is private to our hashtable cpreg; in KVM register
 * IDs the AArch64/32 distinction is the KVM_REG_ARM/ARM64
 * in the upper bits of the 64 bit ID.
 */
#define CP_REG_AA64_SHIFT 28
#define CP_REG_AA64_MASK (1 << CP_REG_AA64_SHIFT)

/* To enable banking of coprocessor registers depending on ns-bit we
 * add a bit to distinguish between secure and non-secure cpregs in the
 * hashtable.
 */
#define CP_REG_NS_SHIFT 29
#define CP_REG_NS_MASK (1 << CP_REG_NS_SHIFT)

#define ENCODE_CP_REG(cp, is64, ns, crn, crm, opc1, opc2)   \
    ((ns) << CP_REG_NS_SHIFT | ((cp) << 16) | ((is64) << 15) |   \
     ((crn) << 11) | ((crm) << 7) | ((opc1) << 3) | (opc2))

#define ENCODE_AA64_CP_REG(cp, crn, crm, op0, op1, op2) \
    (CP_REG_AA64_MASK |                                 \
     ((cp) << CP_REG_ARM_COPROC_SHIFT) |                \
     ((op0) << CP_REG_ARM64_SYSREG_OP0_SHIFT) |         \
     ((op1) << CP_REG_ARM64_SYSREG_OP1_SHIFT) |         \
     ((crn) << CP_REG_ARM64_SYSREG_CRN_SHIFT) |         \
     ((crm) << CP_REG_ARM64_SYSREG_CRM_SHIFT) |         \
     ((op2) << CP_REG_ARM64_SYSREG_OP2_SHIFT))

/* Convert a full 64 bit KVM register ID to the truncated 32 bit
 * version used as a key for the coprocessor register hashtable
 */
static inline uint32_t kvm_to_cpreg_id(uint64_t kvmid)
{
    uint32_t cpregid = kvmid;
    if ((kvmid & CP_REG_ARCH_MASK) == CP_REG_ARM64) {
        cpregid |= CP_REG_AA64_MASK;
    } else {
        if ((kvmid & CP_REG_SIZE_MASK) == CP_REG_SIZE_U64) {
            cpregid |= (1 << 15);
        }

        /* KVM is always non-secure so add the NS flag on AArch32 register
         * entries.
         */
         cpregid |= 1 << CP_REG_NS_SHIFT;
    }
    return cpregid;
}

/* Convert a truncated 32 bit hashtable key into the full
 * 64 bit KVM register ID.
 */
static inline uint64_t cpreg_to_kvm_id(uint32_t cpregid)
{
    uint64_t kvmid;

    if (cpregid & CP_REG_AA64_MASK) {
        kvmid = cpregid & ~CP_REG_AA64_MASK;
        kvmid |= CP_REG_SIZE_U64 | CP_REG_ARM64;
    } else {
        kvmid = cpregid & ~(1 << 15);
        if (cpregid & (1 << 15)) {
            kvmid |= CP_REG_SIZE_U64 | CP_REG_ARM;
        } else {
            kvmid |= CP_REG_SIZE_U32 | CP_REG_ARM;
        }
    }
    return kvmid;
}

/* ARMCPRegInfo type field bits. If the SPECIAL bit is set this is a
 * special-behaviour cp reg and bits [11..8] indicate what behaviour
 * it has. Otherwise it is a simple cp reg, where CONST indicates that
 * TCG can assume the value to be constant (ie load at translate time)
 * and 64BIT indicates a 64 bit wide coprocessor register. SUPPRESS_TB_END
 * indicates that the TB should not be ended after a write to this register
 * (the default is that the TB ends after cp writes). OVERRIDE permits
 * a register definition to override a previous definition for the
 * same (cp, is64, crn, crm, opc1, opc2) tuple: either the new or the
 * old must have the OVERRIDE bit set.
 * ALIAS indicates that this register is an alias view of some underlying
 * state which is also visible via another register, and that the other
 * register is handling migration and reset; registers marked ALIAS will not be
 * migrated but may have their state set by syncing of register state from KVM.
 * NO_RAW indicates that this register has no underlying state and does not
 * support raw access for state saving/loading; it will not be used for either
 * migration or KVM state synchronization. (Typically this is for "registers"
 * which are actually used as instructions for cache maintenance and so on.)
 * IO indicates that this register does I/O and therefore its accesses
 * need to be surrounded by gen_io_start()/gen_io_end(). In particular,
 * registers which implement clocks or timers require this.
 */
#define ARM_CP_SPECIAL           0x0001
#define ARM_CP_CONST             0x0002
#define ARM_CP_64BIT             0x0004
#define ARM_CP_SUPPRESS_TB_END   0x0008
#define ARM_CP_OVERRIDE          0x0010
#define ARM_CP_ALIAS             0x0020
#define ARM_CP_IO                0x0040
#define ARM_CP_NO_RAW            0x0080
#define ARM_CP_NOP               (ARM_CP_SPECIAL | 0x0100)
#define ARM_CP_WFI               (ARM_CP_SPECIAL | 0x0200)
#define ARM_CP_NZCV              (ARM_CP_SPECIAL | 0x0300)
#define ARM_CP_CURRENTEL         (ARM_CP_SPECIAL | 0x0400)
#define ARM_CP_DC_ZVA            (ARM_CP_SPECIAL | 0x0500)
#define ARM_LAST_SPECIAL         ARM_CP_DC_ZVA
#define ARM_CP_FPU               0x1000
#define ARM_CP_SVE               0x2000
/* Used only as a terminator for ARMCPRegInfo lists */
#define ARM_CP_SENTINEL          0xffff
/* Mask of only the flag bits in a type field */
#define ARM_CP_FLAG_MASK         0x30ff

/* Valid values for ARMCPRegInfo state field, indicating which of
 * the AArch32 and AArch64 execution states this register is visible in.
 * If the reginfo doesn't explicitly specify then it is AArch32 only.
 * If the reginfo is declared to be visible in both states then a second
 * reginfo is synthesised for the AArch32 view of the AArch64 register,
 * such that the AArch32 view is the lower 32 bits of the AArch64 one.
 * Note that we rely on the values of these enums as we iterate through
 * the various states in some places.
 */
enum {
    ARM_CP_STATE_AA32 = 0,
    ARM_CP_STATE_AA64 = 1,
    ARM_CP_STATE_BOTH = 2,
};

/* ARM CP register secure state flags.  These flags identify security state
 * attributes for a given CP register entry.
 * The existence of both or neither secure and non-secure flags indicates that
 * the register has both a secure and non-secure hash entry.  A single one of
 * these flags causes the register to only be hashed for the specified
 * security state.
 * Although definitions may have any combination of the S/NS bits, each
 * registered entry will only have one to identify whether the entry is secure
 * or non-secure.
 */
enum {
    ARM_CP_SECSTATE_S =   (1 << 0), /* bit[0]: Secure state register */
    ARM_CP_SECSTATE_NS =  (1 << 1), /* bit[1]: Non-secure state register */
};

/* Return true if cptype is a valid type field. This is used to try to
 * catch errors where the sentinel has been accidentally left off the end
 * of a list of registers.
 */
static inline bool cptype_valid(int cptype)
{
    return ((cptype & ~ARM_CP_FLAG_MASK) == 0)
        || ((cptype & ARM_CP_SPECIAL) &&
            ((cptype & ~ARM_CP_FLAG_MASK) <= ARM_LAST_SPECIAL));
}

/* Access rights:
 * We define bits for Read and Write access for what rev C of the v7-AR ARM ARM
 * defines as PL0 (user), PL1 (fiq/irq/svc/abt/und/sys, ie privileged), and
 * PL2 (hyp). The other level which has Read and Write bits is Secure PL1
 * (ie any of the privileged modes in Secure state, or Monitor mode).
 * If a register is accessible in one privilege level it's always accessible
 * in higher privilege levels too. Since "Secure PL1" also follows this rule
 * (ie anything visible in PL2 is visible in S-PL1, some things are only
 * visible in S-PL1) but "Secure PL1" is a bit of a mouthful, we bend the
 * terminology a little and call this PL3.
 * In AArch64 things are somewhat simpler as the PLx bits line up exactly
 * with the ELx exception levels.
 *
 * If access permissions for a register are more complex than can be
 * described with these bits, then use a laxer set of restrictions, and
 * do the more restrictive/complex check inside a helper function.
 */
#define PL3_R 0x80
#define PL3_W 0x40
#define PL2_R (0x20 | PL3_R)
#define PL2_W (0x10 | PL3_W)
#define PL1_R (0x08 | PL2_R)
#define PL1_W (0x04 | PL2_W)
#define PL0_R (0x02 | PL1_R)
#define PL0_W (0x01 | PL1_W)

#define PL3_RW (PL3_R | PL3_W)
#define PL2_RW (PL2_R | PL2_W)
#define PL1_RW (PL1_R | PL1_W)
#define PL0_RW (PL0_R | PL0_W)

/* Return the highest implemented Exception Level */
static inline int arm_highest_el(CPUARMState *env)
{
    if (arm_feature(env, ARM_FEATURE_EL3)) {
        return 3;
    }
    if (arm_feature(env, ARM_FEATURE_EL2)) {
        return 2;
    }
    return 1;
}

/* Return true if a v7M CPU is in Handler mode */
static inline bool arm_v7m_is_handler_mode(CPUARMState *env)
{
    return env->v7m.exception != 0;
}

/* Return the current Exception Level (as per ARMv8; note that this differs
 * from the ARMv7 Privilege Level).
 */
static inline int arm_current_el(CPUARMState *env)
{
    if (arm_feature(env, ARM_FEATURE_M)) {
        return arm_v7m_is_handler_mode(env) ||
            !(env->v7m.control[env->v7m.secure] & 1);
    }

    if (is_a64(env)) {
        return extract32(env->pstate, 2, 2);
    }

    switch (env->uncached_cpsr & 0x1f) {
    case ARM_CPU_MODE_USR:
        return 0;
    case ARM_CPU_MODE_HYP:
        return 2;
    case ARM_CPU_MODE_MON:
        return 3;
    default:
        if (arm_is_secure(env) && !arm_el_is_aa64(env, 3)) {
            /* If EL3 is 32-bit then all secure privileged modes run in
             * EL3
             */
            return 3;
        }

        return 1;
    }
}

typedef struct ARMCPRegInfo ARMCPRegInfo;

typedef enum CPAccessResult {
    /* Access is permitted */
    CP_ACCESS_OK = 0,
    /* Access fails due to a configurable trap or enable which would
     * result in a categorized exception syndrome giving information about
     * the failing instruction (ie syndrome category 0x3, 0x4, 0x5, 0x6,
     * 0xc or 0x18). The exception is taken to the usual target EL (EL1 or
     * PL1 if in EL0, otherwise to the current EL).
     */
    CP_ACCESS_TRAP = 1,
    /* Access fails and results in an exception syndrome 0x0 ("uncategorized").
     * Note that this is not a catch-all case -- the set of cases which may
     * result in this failure is specifically defined by the architecture.
     */
    CP_ACCESS_TRAP_UNCATEGORIZED = 2,
    /* As CP_ACCESS_TRAP, but for traps directly to EL2 or EL3 */
    CP_ACCESS_TRAP_EL2 = 3,
    CP_ACCESS_TRAP_EL3 = 4,
    /* As CP_ACCESS_UNCATEGORIZED, but for traps directly to EL2 or EL3 */
    CP_ACCESS_TRAP_UNCATEGORIZED_EL2 = 5,
    CP_ACCESS_TRAP_UNCATEGORIZED_EL3 = 6,
    /* Access fails and results in an exception syndrome for an FP access,
     * trapped directly to EL2 or EL3
     */
    CP_ACCESS_TRAP_FP_EL2 = 7,
    CP_ACCESS_TRAP_FP_EL3 = 8,
} CPAccessResult;

/* Access functions for coprocessor registers. These cannot fail and
 * may not raise exceptions.
 */
typedef uint64_t CPReadFn(CPUARMState *env, const ARMCPRegInfo *opaque);
typedef void CPWriteFn(CPUARMState *env, const ARMCPRegInfo *opaque,
                       uint64_t value);
/* Access permission check functions for coprocessor registers. */
typedef CPAccessResult CPAccessFn(CPUARMState *env,
                                  const ARMCPRegInfo *opaque,
                                  bool isread);
/* Hook function for register reset */
typedef void CPResetFn(CPUARMState *env, const ARMCPRegInfo *opaque);

#define CP_ANY 0xff

/* Definition of an ARM coprocessor register */
struct ARMCPRegInfo {
    /* Name of register (useful mainly for debugging, need not be unique) */
    const char *name;
    /* Location of register: coprocessor number and (crn,crm,opc1,opc2)
     * tuple. Any of crm, opc1 and opc2 may be CP_ANY to indicate a
     * 'wildcard' field -- any value of that field in the MRC/MCR insn
     * will be decoded to this register. The register read and write
     * callbacks will be passed an ARMCPRegInfo with the crn/crm/opc1/opc2
     * used by the program, so it is possible to register a wildcard and
     * then behave differently on read/write if necessary.
     * For 64 bit registers, only crm and opc1 are relevant; crn and opc2
     * must both be zero.
     * For AArch64-visible registers, opc0 is also used.
     * Since there are no "coprocessors" in AArch64, cp is purely used as a
     * way to distinguish (for KVM's benefit) guest-visible system registers
     * from demuxed ones provided to preserve the "no side effects on
     * KVM register read/write from QEMU" semantics. cp==0x13 is guest
     * visible (to match KVM's encoding); cp==0 will be converted to
     * cp==0x13 when the ARMCPRegInfo is registered, for convenience.
     */
    uint8_t cp;
    uint8_t crn;
    uint8_t crm;
    uint8_t opc0;
    uint8_t opc1;
    uint8_t opc2;
    /* Execution state in which this register is visible: ARM_CP_STATE_* */
    int state;
    /* Register type: ARM_CP_* bits/values */
    int type;
    /* Access rights: PL*_[RW] */
    int access;
    /* Security state: ARM_CP_SECSTATE_* bits/values */
    int secure;
    /* The opaque pointer passed to define_arm_cp_regs_with_opaque() when
     * this register was defined: can be used to hand data through to the
     * register read/write functions, since they are passed the ARMCPRegInfo*.
     */
    void *opaque;
    /* Value of this register, if it is ARM_CP_CONST. Otherwise, if
     * fieldoffset is non-zero, the reset value of the register.
     */
    uint64_t resetvalue;
    /* Offset of the field in CPUARMState for this register.
     *
     * This is not needed if either:
     *  1. type is ARM_CP_CONST or one of the ARM_CP_SPECIALs
     *  2. both readfn and writefn are specified
     */
    ptrdiff_t fieldoffset; /* offsetof(CPUARMState, field) */

    /* Offsets of the secure and non-secure fields in CPUARMState for the
     * register if it is banked.  These fields are only used during the static
     * registration of a register.  During hashing the bank associated
     * with a given security state is copied to fieldoffset which is used from
     * there on out.
     *
     * It is expected that register definitions use either fieldoffset or
     * bank_fieldoffsets in the definition but not both.  It is also expected
     * that both bank offsets are set when defining a banked register.  This
     * use indicates that a register is banked.
     */
    ptrdiff_t bank_fieldoffsets[2];

    /* Function for making any access checks for this register in addition to
     * those specified by the 'access' permissions bits. If NULL, no extra
     * checks required. The access check is performed at runtime, not at
     * translate time.
     */
    CPAccessFn *accessfn;
    /* Function for handling reads of this register. If NULL, then reads
     * will be done by loading from the offset into CPUARMState specified
     * by fieldoffset.
     */
    CPReadFn *readfn;
    /* Function for handling writes of this register. If NULL, then writes
     * will be done by writing to the offset into CPUARMState specified
     * by fieldoffset.
     */
    CPWriteFn *writefn;
    /* Function for doing a "raw" read; used when we need to copy
     * coprocessor state to the kernel for KVM or out for
     * migration. This only needs to be provided if there is also a
     * readfn and it has side effects (for instance clear-on-read bits).
     */
    CPReadFn *raw_readfn;
    /* Function for doing a "raw" write; used when we need to copy KVM
     * kernel coprocessor state into userspace, or for inbound
     * migration. This only needs to be provided if there is also a
     * writefn and it masks out "unwritable" bits or has write-one-to-clear
     * or similar behaviour.
     */
    CPWriteFn *raw_writefn;
    /* Function for resetting the register. If NULL, then reset will be done
     * by writing resetvalue to the field specified in fieldoffset. If
     * fieldoffset is 0 then no reset will be done.
     */
    CPResetFn *resetfn;
};

/* Macros which are lvalues for the field in CPUARMState for the
 * ARMCPRegInfo *ri.
 */
#define CPREG_FIELD32(env, ri) \
    (*(uint32_t *)((char *)(env) + (ri)->fieldoffset))
#define CPREG_FIELD64(env, ri) \
    (*(uint64_t *)((char *)(env) + (ri)->fieldoffset))

#define REGINFO_SENTINEL { NULL, 0,0,0,0,0,0, 0, ARM_CP_SENTINEL, 0, 0, NULL, 0,0, {0, 0}, 0,0,0,0,0,0, }

void define_arm_cp_regs_with_opaque(ARMCPU *cpu,
                                    const ARMCPRegInfo *regs, void *opaque);
void define_one_arm_cp_reg_with_opaque(ARMCPU *cpu,
                                       const ARMCPRegInfo *regs, void *opaque);
static inline void define_arm_cp_regs(ARMCPU *cpu, const ARMCPRegInfo *regs)
{
    define_arm_cp_regs_with_opaque(cpu, regs, 0);
}
static inline void define_one_arm_cp_reg(ARMCPU *cpu, const ARMCPRegInfo *regs)
{
    define_one_arm_cp_reg_with_opaque(cpu, regs, 0);
}
const ARMCPRegInfo *get_arm_cp_reginfo(GHashTable *cpregs, uint32_t encoded_cp);

/* CPWriteFn that can be used to implement writes-ignored behaviour */
void arm_cp_write_ignore(CPUARMState *env, const ARMCPRegInfo *ri,
                         uint64_t value);
/* CPReadFn that can be used for read-as-zero behaviour */
uint64_t arm_cp_read_zero(CPUARMState *env, const ARMCPRegInfo *ri);

/* CPResetFn that does nothing, for use if no reset is required even
 * if fieldoffset is non zero.
 */
void arm_cp_reset_ignore(CPUARMState *env, const ARMCPRegInfo *opaque);

/* Return true if this reginfo struct's field in the cpu state struct
 * is 64 bits wide.
 */
static inline bool cpreg_field_is_64bit(const ARMCPRegInfo *ri)
{
    return (ri->state == ARM_CP_STATE_AA64) || (ri->type & ARM_CP_64BIT);
}

static inline bool cp_access_ok(int current_el,
                                const ARMCPRegInfo *ri, int isread)
{
    return (ri->access >> ((current_el * 2) + isread)) & 1;
}

/* Raw read of a coprocessor register (as needed for migration, etc) */
uint64_t read_raw_cp_reg(CPUARMState *env, const ARMCPRegInfo *ri);

/**
 * write_list_to_cpustate
 * @cpu: ARMCPU
 *
 * For each register listed in the ARMCPU cpreg_indexes list, write
 * its value from the cpreg_values list into the ARMCPUState structure.
 * This updates TCG's working data structures from KVM data or
 * from incoming migration state.
 *
 * Returns: true if all register values were updated correctly,
 * false if some register was unknown or could not be written.
 * Note that we do not stop early on failure -- we will attempt
 * writing all registers in the list.
 */
bool write_list_to_cpustate(ARMCPU *cpu);

/**
 * write_cpustate_to_list:
 * @cpu: ARMCPU
 *
 * For each register listed in the ARMCPU cpreg_indexes list, write
 * its value from the ARMCPUState structure into the cpreg_values list.
 * This is used to copy info from TCG's working data structures into
 * KVM or for outbound migration.
 *
 * Returns: true if all register values were read correctly,
 * false if some register was unknown or could not be read.
 * Note that we do not stop early on failure -- we will attempt
 * reading all registers in the list.
 */
bool write_cpustate_to_list(ARMCPU *cpu);

#define ARM_CPUID_TI915T      0x54029152
#define ARM_CPUID_TI925T      0x54029252

#if defined(CONFIG_USER_ONLY)
#define TARGET_PAGE_BITS 12
#else
/* The ARM MMU allows 1k pages.  */
/* ??? Linux doesn't actually use these, and they're deprecated in recent
   architecture revisions.  Maybe a configure option to disable them.  */
#define TARGET_PAGE_BITS 10
#endif

#if defined(TARGET_AARCH64)
#  define TARGET_PHYS_ADDR_SPACE_BITS 48
#  define TARGET_VIRT_ADDR_SPACE_BITS 64
#else
#  define TARGET_PHYS_ADDR_SPACE_BITS 40
#  define TARGET_VIRT_ADDR_SPACE_BITS 32
#endif

static inline bool arm_excp_unmasked(CPUState *cs, unsigned int excp_idx,
                                     unsigned int target_el)
{
    CPUARMState *env = cs->env_ptr;
    unsigned int cur_el = arm_current_el(env);
    bool secure = arm_is_secure(env);
    bool pstate_unmasked;
    int8_t unmasked = 0;

    /* Don't take exceptions if they target a lower EL.
     * This check should catch any exceptions that would not be taken but left
     * pending.
     */
    if (cur_el > target_el) {
        return false;
    }

    switch (excp_idx) {
    case EXCP_FIQ:
        pstate_unmasked = !(env->daif & PSTATE_F);
        break;

    case EXCP_IRQ:
        pstate_unmasked = !(env->daif & PSTATE_I);
        break;

    case EXCP_VFIQ:
        if (secure || !(env->cp15.hcr_el2 & HCR_FMO)) {
            /* VFIQs are only taken when hypervized and non-secure.  */
            return false;
        }
        return !(env->daif & PSTATE_F);
    case EXCP_VIRQ:
        if (secure || !(env->cp15.hcr_el2 & HCR_IMO)) {
            /* VIRQs are only taken when hypervized and non-secure.  */
            return false;
        }
        return !(env->daif & PSTATE_I);
    default:
        g_assert_not_reached();
        return false;
    }

    /* Use the target EL, current execution state and SCR/HCR settings to
     * determine whether the corresponding CPSR bit is used to mask the
     * interrupt.
     */
    if ((target_el > cur_el) && (target_el != 1)) {
        /* Exceptions targeting a higher EL may not be maskable */
        if (arm_feature(env, ARM_FEATURE_AARCH64)) {
            /* 64-bit masking rules are simple: exceptions to EL3
             * can't be masked, and exceptions to EL2 can only be
             * masked from Secure state. The HCR and SCR settings
             * don't affect the masking logic, only the interrupt routing.
             */
            if (target_el == 3 || !secure) {
                unmasked = 1;
            }
        } else {
            /* The old 32-bit-only environment has a more complicated
             * masking setup. HCR and SCR bits not only affect interrupt
             * routing but also change the behaviour of masking.
             */
            bool hcr, scr;

            switch (excp_idx) {
            case EXCP_FIQ:
                /* If FIQs are routed to EL3 or EL2 then there are cases where
                 * we override the CPSR.F in determining if the exception is
                 * masked or not. If neither of these are set then we fall back
                 * to the CPSR.F setting otherwise we further assess the state
                 * below.
                 */
                hcr = (env->cp15.hcr_el2 & HCR_FMO);
                scr = (env->cp15.scr_el3 & SCR_FIQ);

                /* When EL3 is 32-bit, the SCR.FW bit controls whether the
                 * CPSR.F bit masks FIQ interrupts when taken in non-secure
                 * state. If SCR.FW is set then FIQs can be masked by CPSR.F
                 * when non-secure but only when FIQs are only routed to EL3.
                 */
                scr = scr && !((env->cp15.scr_el3 & SCR_FW) && !hcr);
                break;
            case EXCP_IRQ:
                /* When EL3 execution state is 32-bit, if HCR.IMO is set then
                 * we may override the CPSR.I masking when in non-secure state.
                 * The SCR.IRQ setting has already been taken into consideration
                 * when setting the target EL, so it does not have a further
                 * affect here.
                 */
                hcr = (env->cp15.hcr_el2 & HCR_IMO);
                scr = false;
                break;
            default:
                g_assert_not_reached();
            }

            if ((scr || hcr) && !secure) {
                unmasked = 1;
            }
        }
    }

    /* The PSTATE bits only mask the interrupt if we have not overriden the
     * ability above.
     */
    return unmasked || pstate_unmasked;
}

static inline CPUARMState *cpu_init(struct uc_struct *uc, const char *cpu_model)
{
    ARMCPU *cpu = cpu_arm_init(uc, cpu_model);
    if (cpu) {
        return &cpu->env;
    }
    return NULL;
}

#ifdef TARGET_ARM
#define cpu_exec cpu_arm_exec

#define ARM_CPU_TYPE_SUFFIX "-" TYPE_ARM_CPU
#define ARM_CPU_TYPE_NAME(name) (name ARM_CPU_TYPE_SUFFIX)

#define cpu_signal_handler cpu_arm_signal_handler
#define cpu_list arm_cpu_list
#endif

/* ARM has the following "translation regimes" (as the ARM ARM calls them):
 *
 * If EL3 is 64-bit:
 *  + NonSecure EL1 & 0 stage 1
 *  + NonSecure EL1 & 0 stage 2
 *  + NonSecure EL2
 *  + Secure EL1 & EL0
 *  + Secure EL3
 * If EL3 is 32-bit:
 *  + NonSecure PL1 & 0 stage 1
 *  + NonSecure PL1 & 0 stage 2
 *  + NonSecure PL2
 *  + Secure PL0 & PL1
 * (reminder: for 32 bit EL3, Secure PL1 is *EL3*, not EL1.)
 *
 * For QEMU, an mmu_idx is not quite the same as a translation regime because:
 *  1. we need to split the "EL1 & 0" regimes into two mmu_idxes, because they
 *     may differ in access permissions even if the VA->PA map is the same
 *  2. we want to cache in our TLB the full VA->IPA->PA lookup for a stage 1+2
 *     translation, which means that we have one mmu_idx that deals with two
 *     concatenated translation regimes [this sort of combined s1+2 TLB is
 *     architecturally permitted]
 *  3. we don't need to allocate an mmu_idx to translations that we won't be
 *     handling via the TLB. The only way to do a stage 1 translation without
 *     the immediate stage 2 translation is via the ATS or AT system insns,
 *     which can be slow-pathed and always do a page table walk.
 *  4. we can also safely fold together the "32 bit EL3" and "64 bit EL3"
 *     translation regimes, because they map reasonably well to each other
 *     and they can't both be active at the same time.
 * This gives us the following list of mmu_idx values:
 *
 * NS EL0 (aka NS PL0) stage 1+2
 * NS EL1 (aka NS PL1) stage 1+2
 * NS EL2 (aka NS PL2)
 * S EL3 (aka S PL1)
 * S EL0 (aka S PL0)
 * S EL1 (not used if EL3 is 32 bit)
 * NS EL0+1 stage 2
 *
 * (The last of these is an mmu_idx because we want to be able to use the TLB
 * for the accesses done as part of a stage 1 page table walk, rather than
 * having to walk the stage 2 page table over and over.)
 *
 * R profile CPUs have an MPU, but can use the same set of MMU indexes
 * as A profile. They only need to distinguish NS EL0 and NS EL1 (and
 * NS EL2 if we ever model a Cortex-R52).
 *
 * M profile CPUs are rather different as they do not have a true MMU.
 * They have the following different MMU indexes:
 *  User
 *  Privileged
 *  User, execution priority negative (ie the MPU HFNMIENA bit may apply)
 *  Privileged, execution priority negative (ditto)
 * If the CPU supports the v8M Security Extension then there are also:
 *  Secure User
 *  Secure Privileged
 *  Secure User, execution priority negative
 *  Secure Privileged, execution priority negative
 *
 * The ARMMMUIdx and the mmu index value used by the core QEMU TLB code
 * are not quite the same -- different CPU types (most notably M profile
 * vs A/R profile) would like to use MMU indexes with different semantics,
 * but since we don't ever need to use all of those in a single CPU we
 * can avoid setting NB_MMU_MODES to more than 8. The lower bits of
 * ARMMMUIdx are the core TLB mmu index, and the higher bits are always
 * the same for any particular CPU.
 * Variables of type ARMMUIdx are always full values, and the core
 * index values are in variables of type 'int'.
 *
 * Our enumeration includes at the end some entries which are not "true"
 * mmu_idx values in that they don't have corresponding TLBs and are only
 * valid for doing slow path page table walks.
 *
 * The constant names here are patterned after the general style of the names
 * of the AT/ATS operations.
 * The values used are carefully arranged to make mmu_idx => EL lookup easy.
 * For M profile we arrange them to have a bit for priv, a bit for negpri
 * and a bit for secure.
 */
#define ARM_MMU_IDX_A 0x10 /* A profile */
#define ARM_MMU_IDX_NOTLB 0x20 /* does not have a TLB */
#define ARM_MMU_IDX_M 0x40 /* M profile */

/* meanings of the bits for M profile mmu idx values */
#define ARM_MMU_IDX_M_PRIV 0x1
#define ARM_MMU_IDX_M_NEGPRI 0x2
#define ARM_MMU_IDX_M_S 0x4

#define ARM_MMU_IDX_TYPE_MASK (~0x7)
#define ARM_MMU_IDX_COREIDX_MASK 0x7

typedef enum ARMMMUIdx {
    ARMMMUIdx_S12NSE0 = 0 | ARM_MMU_IDX_A,
    ARMMMUIdx_S12NSE1 = 1 | ARM_MMU_IDX_A,
    ARMMMUIdx_S1E2 = 2 | ARM_MMU_IDX_A,
    ARMMMUIdx_S1E3 = 3 | ARM_MMU_IDX_A,
    ARMMMUIdx_S1SE0 = 4 | ARM_MMU_IDX_A,
    ARMMMUIdx_S1SE1 = 5 | ARM_MMU_IDX_A,
    ARMMMUIdx_S2NS = 6 | ARM_MMU_IDX_A,
    ARMMMUIdx_MUser = 0 | ARM_MMU_IDX_M,
    ARMMMUIdx_MPriv = 1 | ARM_MMU_IDX_M,
    ARMMMUIdx_MUserNegPri = 2 | ARM_MMU_IDX_M,
    ARMMMUIdx_MPrivNegPri = 3 | ARM_MMU_IDX_M,
    ARMMMUIdx_MSUser = 4 | ARM_MMU_IDX_M,
    ARMMMUIdx_MSPriv = 5 | ARM_MMU_IDX_M,
    ARMMMUIdx_MSUserNegPri = 6 | ARM_MMU_IDX_M,
    ARMMMUIdx_MSPrivNegPri = 7 | ARM_MMU_IDX_M,
    /* Indexes below here don't have TLBs and are used only for AT system
     * instructions or for the first stage of an S12 page table walk.
     */
    ARMMMUIdx_S1NSE0 = 0 | ARM_MMU_IDX_NOTLB,
    ARMMMUIdx_S1NSE1 = 1 | ARM_MMU_IDX_NOTLB,
} ARMMMUIdx;

/* Bit macros for the core-mmu-index values for each index,
 * for use when calling tlb_flush_by_mmuidx() and friends.
 */
typedef enum ARMMMUIdxBit {
    ARMMMUIdxBit_S12NSE0 = 1 << 0,
    ARMMMUIdxBit_S12NSE1 = 1 << 1,
    ARMMMUIdxBit_S1E2 = 1 << 2,
    ARMMMUIdxBit_S1E3 = 1 << 3,
    ARMMMUIdxBit_S1SE0 = 1 << 4,
    ARMMMUIdxBit_S1SE1 = 1 << 5,
    ARMMMUIdxBit_S2NS = 1 << 6,
    ARMMMUIdxBit_MUser = 1 << 0,
    ARMMMUIdxBit_MPriv = 1 << 1,
    ARMMMUIdxBit_MUserNegPri = 1 << 2,
    ARMMMUIdxBit_MPrivNegPri = 1 << 3,
    ARMMMUIdxBit_MSUser = 1 << 4,
    ARMMMUIdxBit_MSPriv = 1 << 5,
    ARMMMUIdxBit_MSUserNegPri = 1 << 6,
    ARMMMUIdxBit_MSPrivNegPri = 1 << 7,
} ARMMMUIdxBit;

#define MMU_USER_IDX 0

static inline int arm_to_core_mmu_idx(ARMMMUIdx mmu_idx)
{
    return mmu_idx & ARM_MMU_IDX_COREIDX_MASK;
}

static inline ARMMMUIdx core_to_arm_mmu_idx(CPUARMState *env, int mmu_idx)
{
    if (arm_feature(env, ARM_FEATURE_M)) {
        return mmu_idx | ARM_MMU_IDX_M;
    } else {
        return mmu_idx | ARM_MMU_IDX_A;
    }
}

/* Return the exception level we're running at if this is our mmu_idx */
static inline int arm_mmu_idx_to_el(ARMMMUIdx mmu_idx)
{
    switch (mmu_idx & ARM_MMU_IDX_TYPE_MASK) {
    case ARM_MMU_IDX_A:
        return mmu_idx & 3;
    case ARM_MMU_IDX_M:
        return mmu_idx & ARM_MMU_IDX_M_PRIV;
    default:
        g_assert_not_reached();
    }
}

/* Return the MMU index for a v7M CPU in the specified security and
 * privilege state
 */
static inline ARMMMUIdx arm_v7m_mmu_idx_for_secstate_and_priv(CPUARMState *env,
                                                              bool secstate,
                                                              bool priv)
{
    ARMMMUIdx mmu_idx = ARM_MMU_IDX_M;

    if (priv) {
        mmu_idx |= ARM_MMU_IDX_M_PRIV;
    }

    // Unicorn: if'd out
#if 0
    if (armv7m_nvic_neg_prio_requested(env->nvic, secstate)) {
        mmu_idx |= ARM_MMU_IDX_M_NEGPRI;
    }
#endif

    if (secstate) {
        mmu_idx |= ARM_MMU_IDX_M_S;
    }

    return mmu_idx;
}

/* Return the MMU index for a v7M CPU in the specified security state */
static inline ARMMMUIdx arm_v7m_mmu_idx_for_secstate(CPUARMState *env,
                                                     bool secstate)
{
    bool priv = arm_current_el(env) != 0;

    return arm_v7m_mmu_idx_for_secstate_and_priv(env, secstate, priv);
}

/* Determine the current mmu_idx to use for normal loads/stores */
static inline int cpu_mmu_index(CPUARMState *env, bool ifetch)
{
    int el = arm_current_el(env);

    if (arm_feature(env, ARM_FEATURE_M)) {
        ARMMMUIdx mmu_idx = arm_v7m_mmu_idx_for_secstate(env, env->v7m.secure);

        return arm_to_core_mmu_idx(mmu_idx);
    }

    if (el < 2 && arm_is_secure_below_el3(env)) {
        return arm_to_core_mmu_idx(ARMMMUIdx_S1SE0 + el);
    }
    return el;
}

/* Indexes used when registering address spaces with cpu_address_space_init */
typedef enum ARMASIdx {
    ARMASIdx_NS = 0,
    ARMASIdx_S = 1,
} ARMASIdx;

/* Return the Exception Level targeted by debug exceptions. */
static inline int arm_debug_target_el(CPUARMState *env)
{
    bool secure = arm_is_secure(env);
    bool route_to_el2 = false;

    if (arm_feature(env, ARM_FEATURE_EL2) && !secure) {
        route_to_el2 = env->cp15.hcr_el2 & HCR_TGE ||
                       env->cp15.mdcr_el2 & (1 << 8);
    }

    if (route_to_el2) {
        return 2;
    } else if (arm_feature(env, ARM_FEATURE_EL3) &&
               !arm_el_is_aa64(env, 3) && secure) {
        return 3;
    } else {
        return 1;
    }
}

static inline bool arm_v7m_csselr_razwi(ARMCPU *cpu)
{
    /* If all the CLIDR.Ctypem bits are 0 there are no caches, and
     * CSSELR is RAZ/WI.
     */
    return (cpu->clidr & R_V7M_CLIDR_CTYPE_ALL_MASK) != 0;
}

static inline bool aa64_generate_debug_exceptions(CPUARMState *env)
{
    if (arm_is_secure(env)) {
        /* MDCR_EL3.SDD disables debug events from Secure state */
        if (extract32(env->cp15.mdcr_el3, 16, 1) != 0
            || arm_current_el(env) == 3) {
            return false;
        }
    }

    if (arm_current_el(env) == arm_debug_target_el(env)) {
        if ((extract32(env->cp15.mdscr_el1, 13, 1) == 0)
            || (env->daif & PSTATE_D)) {
            return false;
        }
    }
    return true;
}

static inline bool aa32_generate_debug_exceptions(CPUARMState *env)
{
    int el = arm_current_el(env);

    if (el == 0 && arm_el_is_aa64(env, 1)) {
        return aa64_generate_debug_exceptions(env);
    }

    if (arm_is_secure(env)) {
        int spd;

        if (el == 0 && (env->cp15.sder & 1)) {
            /* SDER.SUIDEN means debug exceptions from Secure EL0
             * are always enabled. Otherwise they are controlled by
             * SDCR.SPD like those from other Secure ELs.
             */
            return true;
        }

        spd = extract32(env->cp15.mdcr_el3, 14, 2);
        switch (spd) {
        case 1:
            /* SPD == 0b01 is reserved, but behaves as 0b00. */
        case 0:
            /* For 0b00 we return true if external secure invasive debug
             * is enabled. On real hardware this is controlled by external
             * signals to the core. QEMU always permits debug, and behaves
             * as if DBGEN, SPIDEN, NIDEN and SPNIDEN are all tied high.
             */
            return true;
        case 2:
            return false;
        case 3:
            return true;
        }
    }

    return el != 2;
}

/* Return true if debugging exceptions are currently enabled.
 * This corresponds to what in ARM ARM pseudocode would be
 *    if UsingAArch32() then
 *        return AArch32.GenerateDebugExceptions()
 *    else
 *        return AArch64.GenerateDebugExceptions()
 * We choose to push the if() down into this function for clarity,
 * since the pseudocode has it at all callsites except for the one in
 * CheckSoftwareStep(), where it is elided because both branches would
 * always return the same value.
 *
 * Parts of the pseudocode relating to EL2 and EL3 are omitted because we
 * don't yet implement those exception levels or their associated trap bits.
 */
static inline bool arm_generate_debug_exceptions(CPUARMState *env)
{
    if (env->aarch64) {
        return aa64_generate_debug_exceptions(env);
    } else {
        return aa32_generate_debug_exceptions(env);
    }
}

/* Is single-stepping active? (Note that the "is EL_D AArch64?" check
 * implicitly means this always returns false in pre-v8 CPUs.)
 */
static inline bool arm_singlestep_active(CPUARMState *env)
{
    return extract32(env->cp15.mdscr_el1, 0, 1)
        && arm_el_is_aa64(env, arm_debug_target_el(env))
        && arm_generate_debug_exceptions(env);
}

#include "exec/cpu-all.h"

/* Bit usage in the TB flags field: bit 31 indicates whether we are
 * in 32 or 64 bit mode. The meaning of the other bits depends on that.
 * We put flags which are shared between 32 and 64 bit mode at the top
 * of the word, and flags which apply to only one mode at the bottom.
 */
#define ARM_TBFLAG_AARCH64_STATE_SHIFT 31
#define ARM_TBFLAG_AARCH64_STATE_MASK  (1U << ARM_TBFLAG_AARCH64_STATE_SHIFT)
#define ARM_TBFLAG_MMUIDX_SHIFT 28
#define ARM_TBFLAG_MMUIDX_MASK (0x7 << ARM_TBFLAG_MMUIDX_SHIFT)
#define ARM_TBFLAG_SS_ACTIVE_SHIFT 27
#define ARM_TBFLAG_SS_ACTIVE_MASK (1 << ARM_TBFLAG_SS_ACTIVE_SHIFT)
#define ARM_TBFLAG_PSTATE_SS_SHIFT 26
#define ARM_TBFLAG_PSTATE_SS_MASK (1 << ARM_TBFLAG_PSTATE_SS_SHIFT)
/* Target EL if we take a floating-point-disabled exception */
#define ARM_TBFLAG_FPEXC_EL_SHIFT 24
#define ARM_TBFLAG_FPEXC_EL_MASK (0x3 << ARM_TBFLAG_FPEXC_EL_SHIFT)

/* Bit usage when in AArch32 state: */
#define ARM_TBFLAG_THUMB_SHIFT      0
#define ARM_TBFLAG_THUMB_MASK       (1 << ARM_TBFLAG_THUMB_SHIFT)
#define ARM_TBFLAG_VECLEN_SHIFT     1
#define ARM_TBFLAG_VECLEN_MASK      (0x7 << ARM_TBFLAG_VECLEN_SHIFT)
#define ARM_TBFLAG_VECSTRIDE_SHIFT  4
#define ARM_TBFLAG_VECSTRIDE_MASK   (0x3 << ARM_TBFLAG_VECSTRIDE_SHIFT)
#define ARM_TBFLAG_VFPEN_SHIFT      7
#define ARM_TBFLAG_VFPEN_MASK       (1 << ARM_TBFLAG_VFPEN_SHIFT)
#define ARM_TBFLAG_CONDEXEC_SHIFT   8
#define ARM_TBFLAG_CONDEXEC_MASK    (0xff << ARM_TBFLAG_CONDEXEC_SHIFT)
#define ARM_TBFLAG_SCTLR_B_SHIFT    16
#define ARM_TBFLAG_SCTLR_B_MASK     (1 << ARM_TBFLAG_SCTLR_B_SHIFT)
/* We store the bottom two bits of the CPAR as TB flags and handle
 * checks on the other bits at runtime
 */
#define ARM_TBFLAG_XSCALE_CPAR_SHIFT 17
#define ARM_TBFLAG_XSCALE_CPAR_MASK (3 << ARM_TBFLAG_XSCALE_CPAR_SHIFT)
/* Indicates whether cp register reads and writes by guest code should access
 * the secure or nonsecure bank of banked registers; note that this is not
 * the same thing as the current security state of the processor!
 */
#define ARM_TBFLAG_NS_SHIFT         19
#define ARM_TBFLAG_NS_MASK          (1 << ARM_TBFLAG_NS_SHIFT)
#define ARM_TBFLAG_BE_DATA_SHIFT    20
#define ARM_TBFLAG_BE_DATA_MASK     (1 << ARM_TBFLAG_BE_DATA_SHIFT)
/* For M profile only, Handler (ie not Thread) mode */
#define ARM_TBFLAG_HANDLER_SHIFT    21
#define ARM_TBFLAG_HANDLER_MASK     (1 << ARM_TBFLAG_HANDLER_SHIFT)

/* Bit usage when in AArch64 state */
#define ARM_TBFLAG_TBI0_SHIFT 0        /* TBI0 for EL0/1 or TBI for EL2/3 */
#define ARM_TBFLAG_TBI0_MASK (0x1ull << ARM_TBFLAG_TBI0_SHIFT)
#define ARM_TBFLAG_TBI1_SHIFT 1        /* TBI1 for EL0/1  */
#define ARM_TBFLAG_TBI1_MASK (0x1ull << ARM_TBFLAG_TBI1_SHIFT)
#define ARM_TBFLAG_SVEEXC_EL_SHIFT  2
#define ARM_TBFLAG_SVEEXC_EL_MASK   (0x3 << ARM_TBFLAG_SVEEXC_EL_SHIFT)
#define ARM_TBFLAG_ZCR_LEN_SHIFT    4
#define ARM_TBFLAG_ZCR_LEN_MASK     (0xf << ARM_TBFLAG_ZCR_LEN_SHIFT)

/* some convenience accessor macros */
#define ARM_TBFLAG_AARCH64_STATE(F) \
    (((F) & ARM_TBFLAG_AARCH64_STATE_MASK) >> ARM_TBFLAG_AARCH64_STATE_SHIFT)
#define ARM_TBFLAG_MMUIDX(F) \
    (((F) & ARM_TBFLAG_MMUIDX_MASK) >> ARM_TBFLAG_MMUIDX_SHIFT)
#define ARM_TBFLAG_SS_ACTIVE(F) \
    (((F) & ARM_TBFLAG_SS_ACTIVE_MASK) >> ARM_TBFLAG_SS_ACTIVE_SHIFT)
#define ARM_TBFLAG_PSTATE_SS(F) \
    (((F) & ARM_TBFLAG_PSTATE_SS_MASK) >> ARM_TBFLAG_PSTATE_SS_SHIFT)
#define ARM_TBFLAG_FPEXC_EL(F) \
    (((F) & ARM_TBFLAG_FPEXC_EL_MASK) >> ARM_TBFLAG_FPEXC_EL_SHIFT)
#define ARM_TBFLAG_THUMB(F) \
    (((F) & ARM_TBFLAG_THUMB_MASK) >> ARM_TBFLAG_THUMB_SHIFT)
#define ARM_TBFLAG_VECLEN(F) \
    (((F) & ARM_TBFLAG_VECLEN_MASK) >> ARM_TBFLAG_VECLEN_SHIFT)
#define ARM_TBFLAG_VECSTRIDE(F) \
    (((F) & ARM_TBFLAG_VECSTRIDE_MASK) >> ARM_TBFLAG_VECSTRIDE_SHIFT)
#define ARM_TBFLAG_VFPEN(F) \
    (((F) & ARM_TBFLAG_VFPEN_MASK) >> ARM_TBFLAG_VFPEN_SHIFT)
#define ARM_TBFLAG_CONDEXEC(F) \
    (((F) & ARM_TBFLAG_CONDEXEC_MASK) >> ARM_TBFLAG_CONDEXEC_SHIFT)
#define ARM_TBFLAG_SCTLR_B(F) \
    (((F) & ARM_TBFLAG_SCTLR_B_MASK) >> ARM_TBFLAG_SCTLR_B_SHIFT)
#define ARM_TBFLAG_XSCALE_CPAR(F) \
    (((F) & ARM_TBFLAG_XSCALE_CPAR_MASK) >> ARM_TBFLAG_XSCALE_CPAR_SHIFT)
#define ARM_TBFLAG_NS(F) \
    (((F) & ARM_TBFLAG_NS_MASK) >> ARM_TBFLAG_NS_SHIFT)
#define ARM_TBFLAG_BE_DATA(F) \
    (((F) & ARM_TBFLAG_BE_DATA_MASK) >> ARM_TBFLAG_BE_DATA_SHIFT)
#define ARM_TBFLAG_HANDLER(F) \
    (((F) & ARM_TBFLAG_HANDLER_MASK) >> ARM_TBFLAG_HANDLER_SHIFT)
#define ARM_TBFLAG_TBI0(F) \
    (((F) & ARM_TBFLAG_TBI0_MASK) >> ARM_TBFLAG_TBI0_SHIFT)
#define ARM_TBFLAG_TBI1(F) \
    (((F) & ARM_TBFLAG_TBI1_MASK) >> ARM_TBFLAG_TBI1_SHIFT)
#define ARM_TBFLAG_SVEEXC_EL(F) \
    (((F) & ARM_TBFLAG_SVEEXC_EL_MASK) >> ARM_TBFLAG_SVEEXC_EL_SHIFT)
#define ARM_TBFLAG_ZCR_LEN(F) \
    (((F) & ARM_TBFLAG_ZCR_LEN_MASK) >> ARM_TBFLAG_ZCR_LEN_SHIFT)

static inline bool bswap_code(bool sctlr_b)
{
#ifdef CONFIG_USER_ONLY
    /* BE8 (SCTLR.B = 0, TARGET_WORDS_BIGENDIAN = 1) is mixed endian.
     * The invalid combination SCTLR.B=1/CPSR.E=1/TARGET_WORDS_BIGENDIAN=0
     * would also end up as a mixed-endian mode with BE code, LE data.
     */
    return
#ifdef TARGET_WORDS_BIGENDIAN
        1 ^
#endif
        sctlr_b;
#else
    /* All code access in ARM is little endian, and there are no loaders
     * doing swaps that need to be reversed
     */
    return 0;
#endif
}

static inline bool arm_sctlr_b(CPUARMState *env)
{
    return
        /* We need not implement SCTLR.ITD in user-mode emulation, so
         * let linux-user ignore the fact that it conflicts with SCTLR_B.
         * This lets people run BE32 binaries with "-cpu any".
         */
#ifndef CONFIG_USER_ONLY
        !arm_feature(env, ARM_FEATURE_V7) &&
#endif
        (env->cp15.sctlr_el[1] & SCTLR_B) != 0;
}

/* Return true if the processor is in big-endian mode. */
static inline bool arm_cpu_data_is_big_endian(CPUARMState *env)
{
    int cur_el;

    /* In 32bit endianness is determined by looking at CPSR's E bit */
    if (!is_a64(env)) {
        return
#ifdef CONFIG_USER_ONLY
            /* In system mode, BE32 is modelled in line with the
             * architecture (as word-invariant big-endianness), where loads
             * and stores are done little endian but from addresses which
             * are adjusted by XORing with the appropriate constant. So the
             * endianness to use for the raw data access is not affected by
             * SCTLR.B.
             * In user mode, however, we model BE32 as byte-invariant
             * big-endianness (because user-only code cannot tell the
             * difference), and so we need to use a data access endianness
             * that depends on SCTLR.B.
             */
            arm_sctlr_b(env) ||
#endif
                ((env->uncached_cpsr & CPSR_E) ? 1 : 0);
    }

    cur_el = arm_current_el(env);

    if (cur_el == 0) {
        return (env->cp15.sctlr_el[1] & SCTLR_E0E) != 0;
    }

    return (env->cp15.sctlr_el[cur_el] & SCTLR_EE) != 0;
}

#ifndef CONFIG_USER_ONLY
/**
 * arm_regime_tbi0:
 * @env: CPUARMState
 * @mmu_idx: MMU index indicating required translation regime
 *
 * Extracts the TBI0 value from the appropriate TCR for the current EL
 *
 * Returns: the TBI0 value.
 */
uint32_t arm_regime_tbi0(CPUARMState *env, ARMMMUIdx mmu_idx);

/**
 * arm_regime_tbi1:
 * @env: CPUARMState
 * @mmu_idx: MMU index indicating required translation regime
 *
 * Extracts the TBI1 value from the appropriate TCR for the current EL
 *
 * Returns: the TBI1 value.
 */
uint32_t arm_regime_tbi1(CPUARMState *env, ARMMMUIdx mmu_idx);
#else
/* We can't handle tagged addresses properly in user-only mode */
static inline uint32_t arm_regime_tbi0(CPUARMState *env, ARMMMUIdx mmu_idx)
{
    return 0;
}

static inline uint32_t arm_regime_tbi1(CPUARMState *env, ARMMMUIdx mmu_idx)
{
    return 0;
}
#endif

void cpu_get_tb_cpu_state(CPUARMState *env, target_ulong *pc,
                          target_ulong *cs_base, uint32_t *flags);

enum {
    QEMU_PSCI_CONDUIT_DISABLED = 0,
    QEMU_PSCI_CONDUIT_SMC = 1,
    QEMU_PSCI_CONDUIT_HVC = 2,
};

#ifndef CONFIG_USER_ONLY
/* Return the address space index to use for a memory access */
static inline int arm_asidx_from_attrs(CPUState *cs, MemTxAttrs attrs)
{
    return attrs.secure ? ARMASIdx_S : ARMASIdx_NS;
}

/* Return the AddressSpace to use for a memory access
 * (which depends on whether the access is S or NS, and whether
 * the board gave us a separate AddressSpace for S accesses).
 */
static inline AddressSpace *arm_addressspace(CPUState *cs, MemTxAttrs attrs)
{
    return cpu_get_address_space(cs, arm_asidx_from_attrs(cs, attrs));
}
#endif

/**
 * arm_register_el_change_hook:
 * Register a hook function which will be called back whenever this
 * CPU changes exception level or mode. The hook function will be
 * passed a pointer to the ARMCPU and the opaque data pointer passed
 * to this function when the hook was registered.
 *
 * Note that we currently only support registering a single hook function,
 * and will assert if this function is called twice.
 * This facility is intended for the use of the GICv3 emulation.
 */
void arm_register_el_change_hook(ARMCPU *cpu, ARMELChangeHook *hook,
                                 void *opaque);

/**
 * arm_get_el_change_hook_opaque:
 * Return the opaque data that will be used by the el_change_hook
 * for this CPU.
 */
static inline void *arm_get_el_change_hook_opaque(ARMCPU *cpu)
{
    return cpu->el_change_hook_opaque;
}

/**
 * aa32_vfp_dreg:
 * Return a pointer to the Dn register within env in 32-bit mode.
 */
static inline uint64_t *aa32_vfp_dreg(CPUARMState *env, unsigned regno)
{
    return &env->vfp.zregs[regno >> 1].d[regno & 1];
}

/**
 * aa32_vfp_qreg:
 * Return a pointer to the Qn register within env in 32-bit mode.
 */
static inline uint64_t *aa32_vfp_qreg(CPUARMState *env, unsigned regno)
{
    return &env->vfp.zregs[regno].d[0];
}

/**
 * aa64_vfp_qreg:
 * Return a pointer to the Qn register within env in 64-bit mode.
 */
static inline uint64_t *aa64_vfp_qreg(CPUARMState *env, unsigned regno)
{
    return &env->vfp.zregs[regno].d[0];
}

#endif
