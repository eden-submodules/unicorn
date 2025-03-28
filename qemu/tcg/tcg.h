/*
 * Tiny Code Generator for QEMU
 *
 * Copyright (c) 2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef TCG_H
#define TCG_H

#include "qemu-common.h"
#include "cpu.h"
#include "exec/tb-context.h"
#include "qemu/bitops.h"
#include "qemu/queue.h"
#include "tcg-mo.h"
#include "tcg-target.h"
#include "exec/exec-all.h"

#include "uc_priv.h"

/* XXX: make safe guess about sizes */
#define MAX_OP_PER_INSTR 266

#if HOST_LONG_BITS == 32
#define MAX_OPC_PARAM_PER_ARG 2
#else
#define MAX_OPC_PARAM_PER_ARG 1
#endif
#define MAX_OPC_PARAM_IARGS 6
#define MAX_OPC_PARAM_OARGS 1
#define MAX_OPC_PARAM_ARGS (MAX_OPC_PARAM_IARGS + MAX_OPC_PARAM_OARGS)

/* A Call op needs up to 4 + 2N parameters on 32-bit archs,
 * and up to 4 + N parameters on 64-bit archs
 * (N = number of input arguments + output arguments).  */
#define MAX_OPC_PARAM (4 + (MAX_OPC_PARAM_PER_ARG * MAX_OPC_PARAM_ARGS))

#define CPU_TEMP_BUF_NLONGS 128

/* Default target word size to pointer size.  */
#ifndef TCG_TARGET_REG_BITS
# if UINTPTR_MAX == UINT32_MAX
#  define TCG_TARGET_REG_BITS 32
# elif UINTPTR_MAX == UINT64_MAX
#  define TCG_TARGET_REG_BITS 64
# else
#  error Unknown pointer size for tcg target
# endif
#endif

#if TCG_TARGET_REG_BITS == 32
typedef int32_t tcg_target_long;
typedef uint32_t tcg_target_ulong;
#define TCG_PRIlx PRIx32
#define TCG_PRIld PRId32
#elif TCG_TARGET_REG_BITS == 64
typedef int64_t tcg_target_long;
typedef uint64_t tcg_target_ulong;
#define TCG_PRIlx PRIx64
#define TCG_PRIld PRId64
#else
#error unsupported
#endif

/* Oversized TCG guests make things like MTTCG hard
 * as we can't use atomics for cputlb updates.
 */
#if TARGET_LONG_BITS > TCG_TARGET_REG_BITS
#define TCG_OVERSIZED_GUEST 1
#else
#define TCG_OVERSIZED_GUEST 0
#endif

#if TCG_TARGET_NB_REGS <= 32
typedef uint32_t TCGRegSet;
#elif TCG_TARGET_NB_REGS <= 64
typedef uint64_t TCGRegSet;
#else
#error unsupported
#endif

#if TCG_TARGET_REG_BITS == 32
/* Turn some undef macros into false macros.  */
#define TCG_TARGET_HAS_extrl_i64_i32    0
#define TCG_TARGET_HAS_extrh_i64_i32    0
#define TCG_TARGET_HAS_div_i64          0
#define TCG_TARGET_HAS_rem_i64          0
#define TCG_TARGET_HAS_div2_i64         0
#define TCG_TARGET_HAS_rot_i64          0
#define TCG_TARGET_HAS_ext8s_i64        0
#define TCG_TARGET_HAS_ext16s_i64       0
#define TCG_TARGET_HAS_ext32s_i64       0
#define TCG_TARGET_HAS_ext8u_i64        0
#define TCG_TARGET_HAS_ext16u_i64       0
#define TCG_TARGET_HAS_ext32u_i64       0
#define TCG_TARGET_HAS_bswap16_i64      0
#define TCG_TARGET_HAS_bswap32_i64      0
#define TCG_TARGET_HAS_bswap64_i64      0
#define TCG_TARGET_HAS_neg_i64          0
#define TCG_TARGET_HAS_not_i64          0
#define TCG_TARGET_HAS_andc_i64         0
#define TCG_TARGET_HAS_orc_i64          0
#define TCG_TARGET_HAS_eqv_i64          0
#define TCG_TARGET_HAS_nand_i64         0
#define TCG_TARGET_HAS_nor_i64          0
#define TCG_TARGET_HAS_clz_i64          0
#define TCG_TARGET_HAS_ctz_i64          0
#define TCG_TARGET_HAS_ctpop_i64        0
#define TCG_TARGET_HAS_deposit_i64      0
#define TCG_TARGET_HAS_extract_i64      0
#define TCG_TARGET_HAS_sextract_i64     0
#define TCG_TARGET_HAS_movcond_i64      0
#define TCG_TARGET_HAS_add2_i64         0
#define TCG_TARGET_HAS_sub2_i64         0
#define TCG_TARGET_HAS_mulu2_i64        0
#define TCG_TARGET_HAS_muls2_i64        0
#define TCG_TARGET_HAS_muluh_i64        0
#define TCG_TARGET_HAS_mulsh_i64        0
/* Turn some undef macros into true macros.  */
#define TCG_TARGET_HAS_add2_i32         1
#define TCG_TARGET_HAS_sub2_i32         1
#endif

#ifndef TCG_TARGET_deposit_i32_valid
#define TCG_TARGET_deposit_i32_valid(ofs, len) 1
#endif
#ifndef TCG_TARGET_deposit_i64_valid
#define TCG_TARGET_deposit_i64_valid(ofs, len) 1
#endif
#ifndef TCG_TARGET_extract_i32_valid
#define TCG_TARGET_extract_i32_valid(ofs, len) 1
#endif
#ifndef TCG_TARGET_extract_i64_valid
#define TCG_TARGET_extract_i64_valid(ofs, len) 1
#endif

/* Only one of DIV or DIV2 should be defined.  */
#if defined(TCG_TARGET_HAS_div_i32)
#define TCG_TARGET_HAS_div2_i32         0
#elif defined(TCG_TARGET_HAS_div2_i32)
#define TCG_TARGET_HAS_div_i32          0
#define TCG_TARGET_HAS_rem_i32          0
#endif
#if defined(TCG_TARGET_HAS_div_i64)
#define TCG_TARGET_HAS_div2_i64         0
#elif defined(TCG_TARGET_HAS_div2_i64)
#define TCG_TARGET_HAS_div_i64          0
#define TCG_TARGET_HAS_rem_i64          0
#endif

/* For 32-bit targets, some sort of unsigned widening multiply is required.  */
#if TCG_TARGET_REG_BITS == 32 \
    && !(defined(TCG_TARGET_HAS_mulu2_i32) \
         || defined(TCG_TARGET_HAS_muluh_i32))
# error "Missing unsigned widening multiply"
#endif

#if !defined(TCG_TARGET_HAS_v64) \
    && !defined(TCG_TARGET_HAS_v128) \
    && !defined(TCG_TARGET_HAS_v256)
#define TCG_TARGET_MAYBE_vec            0
#define TCG_TARGET_HAS_neg_vec          0
#define TCG_TARGET_HAS_not_vec          0
#define TCG_TARGET_HAS_andc_vec         0
#define TCG_TARGET_HAS_orc_vec          0
#define TCG_TARGET_HAS_shi_vec          0
#define TCG_TARGET_HAS_shs_vec          0
#define TCG_TARGET_HAS_shv_vec          0
#define TCG_TARGET_HAS_mul_vec          0
#else
#define TCG_TARGET_MAYBE_vec            1
#endif
#ifndef TCG_TARGET_HAS_v64
#define TCG_TARGET_HAS_v64              0
#endif
#ifndef TCG_TARGET_HAS_v128
#define TCG_TARGET_HAS_v128             0
#endif
#ifndef TCG_TARGET_HAS_v256
#define TCG_TARGET_HAS_v256             0
#endif

#ifndef TARGET_INSN_START_EXTRA_WORDS
# define TARGET_INSN_START_WORDS 1
#else
# define TARGET_INSN_START_WORDS (1 + TARGET_INSN_START_EXTRA_WORDS)
#endif

typedef enum TCGOpcode {
#define DEF(name, oargs, iargs, cargs, flags) INDEX_op_ ## name,
#include "tcg-opc.h"
#undef DEF
    NB_OPS,
} TCGOpcode;

#define tcg_regset_set_reg(d, r)   ((d) |= (TCGRegSet)1 << (r))
#define tcg_regset_reset_reg(d, r) ((d) &= ~((TCGRegSet)1 << (r)))
#define tcg_regset_test_reg(d, r)  (((d) >> (r)) & 1)

#ifndef TCG_TARGET_INSN_UNIT_SIZE
# error "Missing TCG_TARGET_INSN_UNIT_SIZE"
#elif TCG_TARGET_INSN_UNIT_SIZE == 1
typedef uint8_t tcg_insn_unit;
#elif TCG_TARGET_INSN_UNIT_SIZE == 2
typedef uint16_t tcg_insn_unit;
#elif TCG_TARGET_INSN_UNIT_SIZE == 4
typedef uint32_t tcg_insn_unit;
#elif TCG_TARGET_INSN_UNIT_SIZE == 8
typedef uint64_t tcg_insn_unit;
#else
/* The port better have done this.  */
#endif

#if defined CONFIG_DEBUG_TCG || defined QEMU_STATIC_ANALYSIS
# define tcg_debug_assert(X) do { assert(X); } while (0)
#elif QEMU_GNUC_PREREQ(4, 5)
# define tcg_debug_assert(X) \
    do { if (!(X)) { __builtin_unreachable(); } } while (0)
#else
# define tcg_debug_assert(X) do { (void)(X); } while (0)
#endif

typedef struct TCGRelocation {
    struct TCGRelocation *next;
    int type;
    tcg_insn_unit *ptr;
    intptr_t addend;
} TCGRelocation; 

typedef struct TCGLabel {
    unsigned has_value : 1;
    unsigned id : 31;
    union {
        uintptr_t value;
        tcg_insn_unit *value_ptr;
        TCGRelocation *first_reloc;
    } u;
} TCGLabel;

typedef struct TCGPool {
    struct TCGPool *next;
    int size;
    uint8_t QEMU_ALIGNED(8, data[0]);
} TCGPool;

#define TCG_POOL_CHUNK_SIZE 32768

#define TCG_MAX_TEMPS 512
#define TCG_MAX_INSNS 512

/* when the size of the arguments of a called function is smaller than
   this value, they are statically allocated in the TB stack frame */
#define TCG_STATIC_CALL_ARGS_SIZE 128

typedef enum TCGType {
    TCG_TYPE_I32,
    TCG_TYPE_I64,

    TCG_TYPE_V64,
    TCG_TYPE_V128,
    TCG_TYPE_V256,

    TCG_TYPE_COUNT, /* number of different types */

    /* An alias for the size of the host register.  */
#if TCG_TARGET_REG_BITS == 32
    TCG_TYPE_REG = TCG_TYPE_I32,
#else
    TCG_TYPE_REG = TCG_TYPE_I64,
#endif

    /* An alias for the size of the native pointer.  */
#if UINTPTR_MAX == UINT32_MAX
    TCG_TYPE_PTR = TCG_TYPE_I32,
#else
    TCG_TYPE_PTR = TCG_TYPE_I64,
#endif

    /* An alias for the size of the target "long", aka register.  */
#if TARGET_LONG_BITS == 64
    TCG_TYPE_TL = TCG_TYPE_I64,
#else
    TCG_TYPE_TL = TCG_TYPE_I32,
#endif
} TCGType;

/* Constants for qemu_ld and qemu_st for the Memory Operation field.  */
typedef enum TCGMemOp {
    MO_8     = 0,
    MO_16    = 1,
    MO_32    = 2,
    MO_64    = 3,
    MO_SIZE  = 3,   /* Mask for the above.  */

    MO_SIGN  = 4,   /* Sign-extended, otherwise zero-extended.  */

    MO_BSWAP = 8,   /* Host reverse endian.  */
#ifdef HOST_WORDS_BIGENDIAN
    MO_LE    = MO_BSWAP,
    MO_BE    = 0,
#else
    MO_LE    = 0,
    MO_BE    = MO_BSWAP,
#endif
#ifdef TARGET_WORDS_BIGENDIAN
    MO_TE    = MO_BE,
#else
    MO_TE    = MO_LE,
#endif

    /* MO_UNALN accesses are never checked for alignment.
     * MO_ALIGN accesses will result in a call to the CPU's
     * do_unaligned_access hook if the guest address is not aligned.
     * The default depends on whether the target CPU defines ALIGNED_ONLY.
     *
     * Some architectures (e.g. ARMv8) need the address which is aligned
     * to a size more than the size of the memory access.
     * Some architectures (e.g. SPARCv9) need an address which is aligned,
     * but less strictly than the natural alignment.
     *
     * MO_ALIGN supposes the alignment size is the size of a memory access.
     *
     * than an access size.
     * There are three options:
     * - an alignment to the size of an access (MO_ALIGN);
     * - an alignment to a specified size, which may be more or less than
     *   the access size (MO_ALIGN_x where 'x' is a size in bytes);
     * - unaligned access permitted (MO_UNALN).
     */
    MO_ASHIFT = 4,
    MO_AMASK = 7 << MO_ASHIFT,
#ifdef ALIGNED_ONLY
    MO_ALIGN = 0,
    MO_UNALN = MO_AMASK,
#else
    MO_ALIGN = MO_AMASK,
    MO_UNALN = 0,
#endif
    MO_ALIGN_2  = 1 << MO_ASHIFT,
    MO_ALIGN_4  = 2 << MO_ASHIFT,
    MO_ALIGN_8  = 3 << MO_ASHIFT,
    MO_ALIGN_16 = 4 << MO_ASHIFT,
    MO_ALIGN_32 = 5 << MO_ASHIFT,
    MO_ALIGN_64 = 6 << MO_ASHIFT,

    /* Combinations of the above, for ease of use.  */
    MO_UB    = MO_8,
    MO_UW    = MO_16,
    MO_UL    = MO_32,
    MO_SB    = MO_SIGN | MO_8,
    MO_SW    = MO_SIGN | MO_16,
    MO_SL    = MO_SIGN | MO_32,
    MO_Q     = MO_64,

    MO_LEUW  = MO_LE | MO_UW,
    MO_LEUL  = MO_LE | MO_UL,
    MO_LESW  = MO_LE | MO_SW,
    MO_LESL  = MO_LE | MO_SL,
    MO_LEQ   = MO_LE | MO_Q,

    MO_BEUW  = MO_BE | MO_UW,
    MO_BEUL  = MO_BE | MO_UL,
    MO_BESW  = MO_BE | MO_SW,
    MO_BESL  = MO_BE | MO_SL,
    MO_BEQ   = MO_BE | MO_Q,

    MO_TEUW  = MO_TE | MO_UW,
    MO_TEUL  = MO_TE | MO_UL,
    MO_TESW  = MO_TE | MO_SW,
    MO_TESL  = MO_TE | MO_SL,
    MO_TEQ   = MO_TE | MO_Q,

    MO_SSIZE = MO_SIZE | MO_SIGN,
} TCGMemOp;

/**
 * get_alignment_bits
 * @memop: TCGMemOp value
 *
 * Extract the alignment size from the memop.
 */
static inline unsigned get_alignment_bits(TCGMemOp memop)
{
    unsigned a = memop & MO_AMASK;

    if (a == MO_UNALN) {
        /* No alignment required.  */
        a = 0;
    } else if (a == MO_ALIGN) {
        /* A natural alignment requirement.  */
        a = memop & MO_SIZE;
    } else {
        /* A specific alignment requirement.  */
        a = a >> MO_ASHIFT;
    }
#if defined(CONFIG_SOFTMMU)
    /* The requested alignment cannot overlap the TLB flags.  */
    tcg_debug_assert((TLB_FLAGS_MASK & ((1 << a) - 1)) == 0);
#endif
    return a;
}

typedef tcg_target_ulong TCGArg;

/* Define type and accessor macros for TCG variables.

   TCG variables are the inputs and outputs of TCG ops, as described
   in tcg/README. Target CPU front-end code uses these types to deal
   with TCG variables as it emits TCG code via the tcg_gen_* functions.
   They come in several flavours:
    * TCGv_i32 : 32 bit integer type
    * TCGv_i64 : 64 bit integer type
    * TCGv_ptr : a host pointer type
    * TCGv_vec : a host vector type; the exact size is not exposed
                 to the CPU front-end code.
    * TCGv : an integer type the same size as target_ulong
             (an alias for either TCGv_i32 or TCGv_i64)
   The compiler's type checking will complain if you mix them
   up and pass the wrong sized TCGv to a function.

   Users of tcg_gen_* don't need to know about any of the internal
   details of these, and should treat them as opaque types.
   You won't be able to look inside them in a debugger either.

   Internal implementation details follow:

   Note that there is no definition of the structs TCGv_i32_d etc anywhere.
   This is deliberate, because the values we store in variables of type
   TCGv_i32 are not really pointers-to-structures. They're just small
   integers, but keeping them in pointer types like this means that the
   compiler will complain if you accidentally pass a TCGv_i32 to a
   function which takes a TCGv_i64, and so on. Only the internals of
   TCG need to care about the actual contents of the types.  */

typedef struct TCGv_i32_d *TCGv_i32;
typedef struct TCGv_i64_d *TCGv_i64;
typedef struct TCGv_ptr_d *TCGv_ptr;
typedef struct TCGv_vec_d *TCGv_vec;
typedef TCGv_ptr TCGv_env;
#if TARGET_LONG_BITS == 32
#define TCGv TCGv_i32
#elif TARGET_LONG_BITS == 64
#define TCGv TCGv_i64
#else
#error Unhandled TARGET_LONG_BITS value
#endif

/* call flags */
/* Helper does not read globals (either directly or through an exception). It
   implies TCG_CALL_NO_WRITE_GLOBALS. */
#define TCG_CALL_NO_READ_GLOBALS    0x0010
/* Helper does not write globals */
#define TCG_CALL_NO_WRITE_GLOBALS   0x0020
/* Helper can be safely suppressed if the return value is not used. */
#define TCG_CALL_NO_SIDE_EFFECTS    0x0040

/* convenience version of most used call flags */
#define TCG_CALL_NO_RWG         TCG_CALL_NO_READ_GLOBALS
#define TCG_CALL_NO_WG          TCG_CALL_NO_WRITE_GLOBALS
#define TCG_CALL_NO_SE          TCG_CALL_NO_SIDE_EFFECTS
#define TCG_CALL_NO_RWG_SE      (TCG_CALL_NO_RWG | TCG_CALL_NO_SE)
#define TCG_CALL_NO_WG_SE       (TCG_CALL_NO_WG | TCG_CALL_NO_SE)

/* Used to align parameters.  See the comment before tcgv_i32_temp.  */
#define TCG_CALL_DUMMY_ARG      ((TCGArg)0)

/* Conditions.  Note that these are laid out for easy manipulation by
   the functions below:
     bit 0 is used for inverting;
     bit 1 is signed,
     bit 2 is unsigned,
     bit 3 is used with bit 0 for swapping signed/unsigned.  */
typedef enum {
    /* non-signed */
    TCG_COND_NEVER  = 0 | 0 | 0 | 0,
    TCG_COND_ALWAYS = 0 | 0 | 0 | 1,
    TCG_COND_EQ     = 8 | 0 | 0 | 0,
    TCG_COND_NE     = 8 | 0 | 0 | 1,
    /* signed */
    TCG_COND_LT     = 0 | 0 | 2 | 0,
    TCG_COND_GE     = 0 | 0 | 2 | 1,
    TCG_COND_LE     = 8 | 0 | 2 | 0,
    TCG_COND_GT     = 8 | 0 | 2 | 1,
    /* unsigned */
    TCG_COND_LTU    = 0 | 4 | 0 | 0,
    TCG_COND_GEU    = 0 | 4 | 0 | 1,
    TCG_COND_LEU    = 8 | 4 | 0 | 0,
    TCG_COND_GTU    = 8 | 4 | 0 | 1,
} TCGCond;

/* Invert the sense of the comparison.  */
static inline TCGCond tcg_invert_cond(TCGCond c)
{
    return (TCGCond)(c ^ 1);
}

/* Swap the operands in a comparison.  */
static inline TCGCond tcg_swap_cond(TCGCond c)
{
    return c & 6 ? (TCGCond)(c ^ 9) : c;
}

/* Create an "unsigned" version of a "signed" comparison.  */
static inline TCGCond tcg_unsigned_cond(TCGCond c)
{
    return c & 2 ? (TCGCond)(c ^ 6) : c;
}

/* Create a "signed" version of an "unsigned" comparison.  */
static inline TCGCond tcg_signed_cond(TCGCond c)
{
    return c & 4 ? (TCGCond)(c ^ 6) : c;
}

/* Must a comparison be considered unsigned?  */
static inline bool is_unsigned_cond(TCGCond c)
{
    return (c & 4) != 0;
}

/* Create a "high" version of a double-word comparison.
   This removes equality from a LTE or GTE comparison.  */
static inline TCGCond tcg_high_cond(TCGCond c)
{
    switch (c) {
    case TCG_COND_GE:
    case TCG_COND_LE:
    case TCG_COND_GEU:
    case TCG_COND_LEU:
        return (TCGCond)(c ^ 8);
    default:
        return c;
    }
}

typedef enum TCGTempVal {
    TEMP_VAL_DEAD,
    TEMP_VAL_REG,
    TEMP_VAL_MEM,
    TEMP_VAL_CONST,
} TCGTempVal;

typedef struct TCGTemp {
    TCGReg reg:8;
    TCGTempVal val_type:8;
    TCGType base_type:8;
    TCGType type:8;
    unsigned int fixed_reg:1;
    unsigned int indirect_reg:1;
    unsigned int indirect_base:1;
    unsigned int mem_coherent:1;
    unsigned int mem_allocated:1;
    /* If true, the temp is saved across both basic blocks and
       translation blocks.  */
    unsigned int temp_global:1;
    /* If true, the temp is saved across basic blocks but dead
       at the end of translation blocks.  If false, the temp is
       dead at the end of basic blocks.  */
    unsigned int temp_local:1;
    unsigned int temp_allocated:1;

    tcg_target_long val;
    intptr_t mem_offset;
    struct TCGTemp *mem_base;
    const char *name;

    /* Pass-specific information that can be stored for a temporary.
       One word worth of integer data, and one pointer to data
       allocated separately.  */
    uintptr_t state;
    void *state_ptr;
} TCGTemp;

typedef struct TCGContext TCGContext;

typedef struct TCGTempSet {
    unsigned long l[BITS_TO_LONGS(TCG_MAX_TEMPS)];
} TCGTempSet;

/* While we limit helpers to 6 arguments, for 32-bit hosts, with padding,
   this imples a max of 6*2 (64-bit in) + 2 (64-bit out) = 14 operands.
   There are never more than 2 outputs, which means that we can store all
   dead + sync data within 16 bits.  */
#define DEAD_ARG  4
#define SYNC_ARG  1
typedef uint16_t TCGLifeData;

/* The layout here is designed to avoid a bitfield crossing of
   a 32-bit boundary, which would cause GCC to add extra padding.  */
typedef struct TCGOp {
    TCGOpcode opc   : 8;        /*  8 */

    /* The number of out and in parameter for a call.  */
    /* Parameters for this opcode.  See below.  */
    unsigned param1 : 4;        /* 12 */
    unsigned param2 : 4;        /* 16 */

    /* Lifetime data of the operands.  */
    unsigned life   : 16;       /* 32 */

    /* Next and previous opcodes.  */
    QTAILQ_ENTRY(TCGOp) link;

    /* Arguments for the opcode.  */
    TCGArg args[MAX_OPC_PARAM];
} TCGOp;

#define TCGOP_CALLI(X)    (X)->param1
#define TCGOP_CALLO(X)    (X)->param2

#define TCGOP_VECL(X)     (X)->param1
#define TCGOP_VECE(X)     (X)->param2

/* Make sure operands fit in the bitfields above.  */
QEMU_BUILD_BUG_ON(NB_OPS > (1 << 8));

/* pool based memory allocation */

/* user-mode: tb_lock must be held for tcg_malloc_internal. */
void *tcg_malloc_internal(TCGContext *s, int size);
void tcg_pool_reset(TCGContext *s);
TranslationBlock *tcg_tb_alloc(TCGContext *s);

void tcg_context_init(struct uc_struct *uc, TCGContext *s);
void tcg_context_free(void *s);   // free memory allocated for @s
void tcg_register_thread(struct uc_struct *uc);
void tcg_prologue_init(TCGContext *s);
void tcg_func_start(TCGContext *s);

int tcg_gen_code(TCGContext *s, TranslationBlock *tb);

void tcg_set_frame(TCGContext *s, TCGReg reg, intptr_t start, intptr_t size);

#if defined(CONFIG_DEBUG_TCG)
/* If you call tcg_clear_temp_count() at the start of a section of
 * code which is not supposed to leak any TCG temporaries, then
 * calling tcg_check_temp_count() at the end of the section will
 * return 1 if the section did in fact leak a temporary.
 */
void tcg_clear_temp_count(void);
int tcg_check_temp_count(void);
#else
#define tcg_clear_temp_count() do { } while (0)
#define tcg_check_temp_count() 0
#endif

void tcg_dump_info(FILE *f, fprintf_function cpu_fprintf);

#define TCG_CT_ALIAS  0x80
#define TCG_CT_IALIAS 0x40
#define TCG_CT_NEWREG 0x20 /* output requires a new register */
#define TCG_CT_REG    0x01
#define TCG_CT_CONST  0x02 /* any constant of register size */

typedef struct TCGArgConstraint {
    uint16_t ct;
    uint8_t alias_index;
    union {
        TCGRegSet regs;
    } u;
} TCGArgConstraint;

#define TCG_MAX_OP_ARGS 16

/* Bits for TCGOpDef->flags, 8 bits available.  */
enum {
    /* Instruction defines the end of a basic block.  */
    TCG_OPF_BB_END       = 0x01,
    /* Instruction clobbers call registers and potentially update globals.  */
    TCG_OPF_CALL_CLOBBER = 0x02,
    /* Instruction has side effects: it cannot be removed if its outputs
       are not used, and might trigger exceptions.  */
    TCG_OPF_SIDE_EFFECTS = 0x04,
    /* Instruction operands are 64-bits (otherwise 32-bits).  */
    TCG_OPF_64BIT        = 0x08,
    /* Instruction is optional and not implemented by the host, or insn
       is generic and should not be implemened by the host.  */
    TCG_OPF_NOT_PRESENT  = 0x10,
    /* Instruction operands are vectors.  */
    TCG_OPF_VECTOR       = 0x20,
};

typedef struct TCGOpDef {
    const char *name;
    uint8_t nb_oargs, nb_iargs, nb_cargs, nb_args;
    uint8_t flags;
    TCGArgConstraint *args_ct;
    int *sorted_args;
#if defined(CONFIG_DEBUG_TCG)
    int used;
#endif
} TCGOpDef;

struct tcg_temp_info {
    bool is_const;
    TCGTemp *prev_copy;
    TCGTemp *next_copy;
    tcg_target_ulong val;
    tcg_target_ulong mask;
};

struct TCGContext {
    uint8_t *pool_cur, *pool_end;
    TCGPool *pool_first, *pool_current, *pool_first_large;
    int nb_labels;
    int nb_globals;
    int nb_temps;
    int nb_indirects;

    /* goto_tb support */
    tcg_insn_unit *code_buf;
    uint16_t *tb_jmp_reset_offset; /* tb->jmp_reset_offset */
    uintptr_t *tb_jmp_insn_offset; /* tb->jmp_target_arg if direct_jump */
    uintptr_t *tb_jmp_target_addr; /* tb->jmp_target_arg if !direct_jump */

    TCGRegSet reserved_regs;
    uint32_t tb_cflags; /* cflags of the current TB */
    intptr_t current_frame_offset;
    intptr_t frame_start;
    intptr_t frame_end;
    TCGTemp *frame_temp;

    tcg_insn_unit *code_ptr;

    GHashTable *helpers;

#ifdef CONFIG_PROFILER
    /* profiling info */
    int64_t tb_count1;
    int64_t tb_count;
    int64_t op_count; /* total insn count */
    int op_count_max; /* max insn per TB */
    int64_t temp_count;
    int temp_count_max;
    int64_t del_op_count;
    int64_t code_in_len;
    int64_t code_out_len;
    int64_t search_out_len;
    int64_t interm_time;
    int64_t code_time;
    int64_t la_time;
    int64_t opt_time;
    int64_t restore_count;
    int64_t restore_time;
#endif

#ifdef CONFIG_DEBUG_TCG
    int temps_in_use;
    int goto_tb_issue_mask;
#endif

    /* Code generation.  Note that we specifically do not use tcg_insn_unit
       here, because there's too much arithmetic throughout that relies
       on addition and subtraction working on bytes.  Rely on the GCC
       extension that allows arithmetic on void*.  */
    void *code_gen_prologue;
    void *code_gen_epilogue;
    void *code_gen_buffer;
    size_t code_gen_buffer_size;
    void *code_gen_ptr;
    void *data_gen_ptr;

    /* Threshold to flush the translated code buffer.  */
    void *code_gen_highwater;

    /* Track which vCPU triggers events */
    CPUState *cpu;                      /* *_trans */

    /* These structures are private to tcg-target.inc.c.  */
#ifdef TCG_TARGET_NEED_LDST_LABELS
    struct TCGLabelQemuLdst *ldst_labels;
#endif
#ifdef TCG_TARGET_NEED_POOL_LABELS
    struct TCGLabelPoolData *pool_labels;
#endif

    TCGTempSet free_temps[TCG_TYPE_COUNT * 2];
    TCGTemp temps[TCG_MAX_TEMPS]; /* globals first, temps after */

    QTAILQ_HEAD(TCGOpHead, TCGOp) ops, free_ops;

    /* Tells which temporary holds a given register.
       It does not take into account fixed registers */
    TCGTemp *reg_to_temp[TCG_TARGET_NB_REGS];

    target_ulong gen_opc_pc[OPC_BUF_SIZE];
    uint16_t gen_opc_icount[OPC_BUF_SIZE];
    uint8_t gen_opc_instr_start[OPC_BUF_SIZE];

    uint16_t gen_insn_end_off[TCG_MAX_INSNS];
    target_ulong gen_insn_data[TCG_MAX_INSNS][TARGET_INSN_START_WORDS];

    // Unicorn engine variables
    struct uc_struct *uc;
    /* qemu/target-i386/translate.c: global register indexes */
    TCGv_i32 cpu_cc_op;
    TCGv cpu_regs[16]; // 16 GRP for X86-64
    TCGv cpu_seg_base[6];
    TCGv_i64 cpu_bndl[4];
    TCGv_i64 cpu_bndu[4];
    int x86_64_hregs;   // qemu/target-i386/translate.c

    /* qemu/target-i386/translate.c: global TCGv vars */
    TCGv cpu_A0;
    TCGv cpu_cc_dst;
    TCGv cpu_cc_src;
    TCGv cpu_cc_src2;
    TCGv cpu_cc_srcT;

    /* qemu/target-i386/translate.c: local temps */
    TCGv cpu_T0;
    TCGv cpu_T1;

    /* qemu/target-i386/translate.c: local register indexes (only used inside old micro ops) */
    TCGv cpu_tmp0, cpu_tmp4;
    TCGv_ptr cpu_ptr0, cpu_ptr1;
    TCGv_i32 cpu_tmp2_i32, cpu_tmp3_i32;
    TCGv_i64 cpu_tmp1_i64;

    /* qemu/tcg/i386/tcg-target.c */
    void *tb_ret_addr;
    int guest_base_flags;
    /* If bit_MOVBE is defined in cpuid.h (added in GCC version 4.6), we are
       going to attempt to determine at runtime whether movbe is available.  */
    bool have_movbe;

    /* qemu/tcg/tcg.c */
    uint64_t tcg_target_call_clobber_regs;
    uint64_t tcg_target_available_regs[TCG_TYPE_COUNT];
    // Unicorn: Use a large array size to get around needing a file static
    //          Initially was using: ARRAY_SIZE(tcg_target_reg_alloc_order) as the size
    int indirect_reg_alloc_order[50];
    TCGOpDef *tcg_op_defs;

    /* qemu/target-m68k/translate.c */
    TCGv_i32 cpu_halted;
    char cpu_reg_names[2 * 8 * 3 + 5 * 4];
    TCGv cpu_dregs[8];
    TCGv cpu_aregs[8];
    TCGv_i64 cpu_macc[4];
    TCGv QREG_PC;
    TCGv QREG_SR;
    TCGv QREG_CC_OP;
    TCGv QREG_CC_X;
    TCGv QREG_CC_C;
    TCGv QREG_CC_N;
    TCGv QREG_CC_V;
    TCGv QREG_CC_Z;
    TCGv QREG_MACSR;
    TCGv QREG_MAC_MASK;
    TCGv NULL_QREG;
    void *opcode_table[65536];
    /* Used to distinguish stores from bad addressing modes.  */
    TCGv store_dummy;

    /* qemu/target-arm/translate.c */
    TCGv_i64 cpu_V0, cpu_V1, cpu_M0;
    /* We reuse the same 64-bit temporaries for efficiency.  */
    TCGv_i32 cpu_R[16];
    TCGv_i32 cpu_CF, cpu_NF, cpu_VF, cpu_ZF;
    TCGv_i64 cpu_exclusive_addr;
    TCGv_i64 cpu_exclusive_val;
    TCGv_i32 cpu_F0s, cpu_F1s;
    TCGv_i64 cpu_F0d, cpu_F1d;

    /* qemu/target-arm/translate-a64.c */
    TCGv_i64 cpu_pc;
    /* Load/store exclusive handling */
    TCGv_i64 cpu_exclusive_high;
    TCGv_i64 cpu_X[32];

    /* qemu/target-mips/translate.c */
    /* global register indices */
    TCGv cpu_gpr[32];
    TCGv cpu_PC;
    TCGv cpu_HI[4], cpu_LO[4];    // MIPS_DSP_ACC = 4 in qemu/target-mips/cpu.h
    TCGv cpu_dspctrl;
    TCGv btarget;
    TCGv bcond;
    TCGv_i32 hflags;
    TCGv_i32 fpu_fcr31;
    TCGv_i64 fpu_f64[32];
    TCGv_i64 msa_wr_d[64];

    /* qemu/target-sparc/translate.c */
    /* global register indexes */
    TCGv_ptr cpu_regwptr;
    TCGv_i32 cpu_psr;
    TCGv_i32 cpu_xcc, cpu_fprs;
    /* Floating point registers */
    TCGv_i64 cpu_fpr[32];   // TARGET_DPREGS = 32 for Sparc64, 16 for Sparc

    TCGv cpu_fsr;
    TCGv sparc_cpu_pc;
    TCGv cpu_npc;
    TCGv cpu_regs_sparc[32];
    TCGv cpu_y;
    TCGv cpu_tbr;
    TCGv cpu_cond;
    TCGv cpu_gsr;
    TCGv cpu_tick_cmpr;
    TCGv cpu_stick_cmpr;
    TCGv cpu_hstick_cmpr;
    TCGv cpu_hintp;
    TCGv cpu_htba;
    TCGv cpu_hver;
    TCGv cpu_ssr;
    TCGv cpu_ver;
    TCGv cpu_wim;

    TCGLabel *exitreq_label;  // gen_tb_start()
};

static inline size_t temp_idx(TCGContext *tcg_ctx, TCGTemp *ts)
{
    ptrdiff_t n = ts - tcg_ctx->temps;
    tcg_debug_assert(n >= 0 && n < tcg_ctx->nb_temps);
    return n;
}

static inline TCGArg temp_arg(TCGTemp *ts)
{
    return (uintptr_t)ts;
}

static inline TCGTemp *arg_temp(TCGArg a)
{
    return (TCGTemp *)(uintptr_t)a;
}

/* Using the offset of a temporary, relative to TCGContext, rather than
   its index means that we don't use 0.  That leaves offset 0 free for
   a NULL representation without having to leave index 0 unused.  */
static inline TCGTemp *tcgv_i32_temp(TCGContext *s, TCGv_i32 v)
{
    uintptr_t o = (uintptr_t)v;
    TCGTemp *t = (void *)s + o;
    tcg_debug_assert(offsetof(TCGContext, temps[temp_idx(s, t)]) == o);
    return t;
}

static inline TCGTemp *tcgv_i64_temp(TCGContext *s, TCGv_i64 v)
{
    return tcgv_i32_temp(s, (TCGv_i32)v);
}

static inline TCGTemp *tcgv_ptr_temp(TCGContext *s, TCGv_ptr v)
{
    return tcgv_i32_temp(s, (TCGv_i32)v);
}

static inline TCGTemp *tcgv_vec_temp(TCGContext *s, TCGv_vec v)
{
    return tcgv_i32_temp(s, (TCGv_i32)v);
}

static inline TCGArg tcgv_i32_arg(TCGContext *s, TCGv_i32 v)
{
    return temp_arg(tcgv_i32_temp(s, v));
}

static inline TCGArg tcgv_i64_arg(TCGContext *s, TCGv_i64 v)
{
    return temp_arg(tcgv_i64_temp(s, v));
}

static inline TCGArg tcgv_ptr_arg(TCGContext *s, TCGv_ptr v)
{
    return temp_arg(tcgv_ptr_temp(s, v));
}

static inline TCGArg tcgv_vec_arg(TCGContext *s, TCGv_vec v)
{
    return temp_arg(tcgv_vec_temp(s, v));
}

static inline TCGv_i32 temp_tcgv_i32(TCGContext *s, TCGTemp *t)
{
    (void)temp_idx(s, t); /* trigger embedded assert */
    return (TCGv_i32)((void *)t - (void *)s);
}

static inline TCGv_i64 temp_tcgv_i64(TCGContext *s, TCGTemp *t)
{
    return (TCGv_i64)temp_tcgv_i32(s, t);
}

static inline TCGv_ptr temp_tcgv_ptr(TCGContext *s, TCGTemp *t)
{
    return (TCGv_ptr)temp_tcgv_i32(s, t);
}

static inline TCGv_vec temp_tcgv_vec(TCGContext *s, TCGTemp *t)
{
    return (TCGv_vec)temp_tcgv_i32(s, t);
}

#if TCG_TARGET_REG_BITS == 32
static inline TCGv_i32 TCGV_LOW(TCGContext *s, TCGv_i64 t)
{
    return temp_tcgv_i32(s, tcgv_i64_temp(s, t));
}

static inline TCGv_i32 TCGV_HIGH(TCGContext *s, TCGv_i64 t)
{
    return temp_tcgv_i32(s, tcgv_i64_temp(s, t) + 1);
}
#endif

static inline void tcg_set_insn_param(TCGOp *op, int arg, TCGArg v)
{
    op->args[arg] = v;
}

/* The last op that was emitted.  */
static inline TCGOp *tcg_last_op(TCGContext *tcg_ctx)
{
    return QTAILQ_LAST(&tcg_ctx->ops, TCGOpHead);
}

/* Test for whether to terminate the TB for using too many opcodes.  */
static inline bool tcg_op_buf_full(TCGContext *tcg_ctx)
{
    return false;
}

TCGTemp *tcg_global_mem_new_internal(TCGContext *s, TCGType type, TCGv_ptr base,
                                     intptr_t offset, const char *name);

TCGv_i32 tcg_temp_new_internal_i32(TCGContext *s, int temp_local);
TCGv_i64 tcg_temp_new_internal_i64(TCGContext *s, int temp_local);
TCGv_vec tcg_temp_new_vec(TCGContext *s, TCGType type);
TCGv_vec tcg_temp_new_vec_matching(TCGContext *s, TCGv_vec match);

void tcg_temp_free_i32(TCGContext *s, TCGv_i32 arg);
void tcg_temp_free_i64(TCGContext *s, TCGv_i64 arg);
void tcg_temp_free_vec(TCGContext *s, TCGv_vec arg);

static inline TCGv_i32 tcg_global_mem_new_i32(TCGContext *s, TCGv_ptr reg,
                                              intptr_t offset, const char *name)
{
    TCGTemp *t = tcg_global_mem_new_internal(s, TCG_TYPE_I32, reg, offset, name);
    return temp_tcgv_i32(s, t);
}

static inline TCGv_i32 tcg_temp_new_i32(TCGContext *s)
{
    return tcg_temp_new_internal_i32(s, 0);
}

static inline TCGv_i32 tcg_temp_local_new_i32(TCGContext *s)
{
    return tcg_temp_new_internal_i32(s, 1);
}

static inline TCGv_i64 tcg_global_mem_new_i64(TCGContext *s, TCGv_ptr reg,
                                              intptr_t offset, const char *name)
{
    TCGTemp *t = tcg_global_mem_new_internal(s, TCG_TYPE_I64, reg, offset, name);
    return temp_tcgv_i64(s, t);
}

static inline TCGv_i64 tcg_temp_new_i64(TCGContext *s)
{
    return tcg_temp_new_internal_i64(s, 0);
}

static inline TCGv_i64 tcg_temp_local_new_i64(TCGContext *s)
{
    return tcg_temp_new_internal_i64(s, 1);
}

// UNICORN: Added
#define TCG_OP_DEFS_TABLE_SIZE 171
extern const TCGOpDef tcg_op_defs_org[TCG_OP_DEFS_TABLE_SIZE];

typedef struct TCGTargetOpDef {
    TCGOpcode op;
    const char *args_ct_str[TCG_MAX_OP_ARGS];
} TCGTargetOpDef;

#define tcg_abort() \
do {\
    fprintf(stderr, "%s:%d: tcg fatal error\n", __FILE__, __LINE__);\
    abort();\
} while (0)

#if UINTPTR_MAX == UINT32_MAX
static inline TCGv_ptr TCGV_NAT_TO_PTR(TCGv_i32 n) { return (TCGv_ptr)n; }
static inline TCGv_i32 TCGV_PTR_TO_NAT(TCGv_ptr n) { return (TCGv_i32)n; }

#define tcg_const_ptr(t, V) TCGV_NAT_TO_PTR(tcg_const_i32(t, (intptr_t)(V)))
#define tcg_global_mem_new_ptr(t, R, O, N) \
    TCGV_NAT_TO_PTR(tcg_global_mem_new_i32(t, (R), (O), (N)))
#define tcg_temp_new_ptr(s) TCGV_NAT_TO_PTR(tcg_temp_new_i32(s))
#define tcg_temp_free_ptr(s, T) tcg_temp_free_i32(s, TCGV_PTR_TO_NAT(T))
#else
static inline TCGv_ptr TCGV_NAT_TO_PTR(TCGv_i64 n) { return (TCGv_ptr)n; }
static inline TCGv_i64 TCGV_PTR_TO_NAT(TCGv_ptr n) { return (TCGv_i64)n; }

#define tcg_const_ptr(t, V) TCGV_NAT_TO_PTR(tcg_const_i64(t, (intptr_t)(V)))
#define tcg_global_mem_new_ptr(t, R, O, N) \
    TCGV_NAT_TO_PTR(tcg_global_mem_new_i64(t, (R), (O), (N)))
#define tcg_temp_new_ptr(s) TCGV_NAT_TO_PTR(tcg_temp_new_i64(s))
#define tcg_temp_free_ptr(s, T) tcg_temp_free_i64(s, TCGV_PTR_TO_NAT(T))
#endif

bool tcg_op_supported(TCGOpcode op);

void tcg_gen_callN(TCGContext *s, void *func, TCGTemp *ret, int nargs, TCGTemp **args);

TCGOp *tcg_emit_op(TCGContext *s, TCGOpcode opc);
void tcg_op_remove(TCGContext *s, TCGOp *op);
TCGOp *tcg_op_insert_before(TCGContext *s, TCGOp *op, TCGOpcode opc, int narg);
TCGOp *tcg_op_insert_after(TCGContext *s, TCGOp *op, TCGOpcode opc, int narg);

void tcg_optimize(TCGContext *s);

void tcg_region_init(struct uc_struct *uc);
void tcg_region_reset_all(struct uc_struct *uc);

size_t tcg_code_size(struct uc_struct *uc);
size_t tcg_code_capacity(struct uc_struct *uc);

/* user-mode: Called with tb_lock held.  */
static inline void *tcg_malloc(TCGContext *s, int size)
{
    uint8_t *ptr, *ptr_end;

    /* ??? This is a weak placeholder for minimum malloc alignment.  */
    size = QEMU_ALIGN_UP(size, 8);

    ptr = s->pool_cur;
    ptr_end = ptr + size;
    if (unlikely(ptr_end > s->pool_end)) {
        return tcg_malloc_internal(s, size);
    } else {
        s->pool_cur = ptr_end;
        return ptr;
    }
}

/* only used for debugging purposes */
void tcg_dump_ops(TCGContext *s);

TCGv_i32 tcg_const_i32(TCGContext *s, int32_t val);
TCGv_i64 tcg_const_i64(TCGContext *s, int64_t val);
TCGv_i32 tcg_const_local_i32(TCGContext *s, int32_t val);
TCGv_i64 tcg_const_local_i64(TCGContext *s, int64_t val);
TCGv_vec tcg_const_zeros_vec(TCGContext *s, TCGType);
TCGv_vec tcg_const_ones_vec(TCGContext *s, TCGType);
TCGv_vec tcg_const_zeros_vec_matching(TCGContext *s, TCGv_vec);
TCGv_vec tcg_const_ones_vec_matching(TCGContext *s, TCGv_vec);

TCGLabel *gen_new_label(TCGContext* s);

/**
 * label_arg
 * @l: label
 *
 * Encode a label for storage in the TCG opcode stream.
 */

static inline TCGArg label_arg(TCGContext *tcg_ctx, TCGLabel *l)
{
    return (uintptr_t)l;
}

/**
 * arg_label
 * @i: value
 *
 * The opposite of label_arg.  Retrieve a label from the
 * encoding of the TCG opcode stream.
 */

static inline TCGLabel *arg_label(TCGContext *tcg_ctx, TCGArg i)
{
    return (TCGLabel *)(uintptr_t)i;
}

/**
 * tcg_ptr_byte_diff
 * @a, @b: addresses to be differenced
 *
 * There are many places within the TCG backends where we need a byte
 * difference between two pointers.  While this can be accomplished
 * with local casting, it's easy to get wrong -- especially if one is
 * concerned with the signedness of the result.
 *
 * This version relies on GCC's void pointer arithmetic to get the
 * correct result.
 */

static inline ptrdiff_t tcg_ptr_byte_diff(void *a, void *b)
{
    return (char*)a - (char*)b;
}

/**
 * tcg_pcrel_diff
 * @s: the tcg context
 * @target: address of the target
 *
 * Produce a pc-relative difference, from the current code_ptr
 * to the destination address.
 */

static inline ptrdiff_t tcg_pcrel_diff(TCGContext *s, void *target)
{
    return tcg_ptr_byte_diff(target, s->code_ptr);
}

/**
 * tcg_current_code_size
 * @s: the tcg context
 *
 * Compute the current code size within the translation block.
 * This is used to fill in qemu's data structures for goto_tb.
 */

static inline size_t tcg_current_code_size(TCGContext *s)
{
    return tcg_ptr_byte_diff(s->code_ptr, s->code_buf);
}

/* Combine the TCGMemOp and mmu_idx parameters into a single value.  */
typedef uint32_t TCGMemOpIdx;

/**
 * make_memop_idx
 * @op: memory operation
 * @idx: mmu index
 *
 * Encode these values into a single parameter.
 */
static inline TCGMemOpIdx make_memop_idx(TCGMemOp op, unsigned idx)
{
    tcg_debug_assert(idx <= 15);
    return (op << 4) | idx;
}

/**
 * get_memop
 * @oi: combined op/idx parameter
 *
 * Extract the memory operation from the combined value.
 */
static inline TCGMemOp get_memop(TCGMemOpIdx oi)
{
    return oi >> 4;
}

/**
 * get_mmuidx
 * @oi: combined op/idx parameter
 *
 * Extract the mmu index from the combined value.
 */
static inline unsigned get_mmuidx(TCGMemOpIdx oi)
{
    return oi & 15;
}

/**
 * tcg_qemu_tb_exec:
 * @env: pointer to CPUArchState for the CPU
 * @tb_ptr: address of generated code for the TB to execute
 *
 * Start executing code from a given translation block.
 * Where translation blocks have been linked, execution
 * may proceed from the given TB into successive ones.
 * Control eventually returns only when some action is needed
 * from the top-level loop: either control must pass to a TB
 * which has not yet been directly linked, or an asynchronous
 * event such as an interrupt needs handling.
 *
 * Return: The return value is the value passed to the corresponding
 * tcg_gen_exit_tb() at translation time of the last TB attempted to execute.
 * The value is either zero or a 4-byte aligned pointer to that TB combined
 * with additional information in its two least significant bits. The
 * additional information is encoded as follows:
 *  0, 1: the link between this TB and the next is via the specified
 *        TB index (0 or 1). That is, we left the TB via (the equivalent
 *        of) "goto_tb <index>". The main loop uses this to determine
 *        how to link the TB just executed to the next.
 *  2:    we are using instruction counting code generation, and we
 *        did not start executing this TB because the instruction counter
 *        would hit zero midway through it. In this case the pointer
 *        returned is the TB we were about to execute, and the caller must
 *        arrange to execute the remaining count of instructions.
 *  3:    we stopped because the CPU's exit_request flag was set
 *        handled). The pointer returned is the TB we were about to execute
 *        when we noticed the pending exit request.
 *        about to execute when we noticed the pending exit request.
 *
 * If the bottom two bits indicate an exit-via-index then the CPU
 * state is correctly synchronised and ready for execution of the next
 * TB (and in particular the guest PC is the address to execute next).
 * Otherwise, we gave up on execution of this TB before it started, and
 * the caller must fix up the CPU state by calling the CPU's
 * synchronize_from_tb() method with the TB pointer we return (falling
 * back to calling the CPU's set_pc method with tb->pb if no
 * synchronize_from_tb() method exists).
 *
 * Note that TCG targets may use a different definition of tcg_qemu_tb_exec
 * to this default (which just calls the prologue.code emitted by
 * tcg_target_qemu_prologue()).
 */
#define TB_EXIT_MASK 3
#define TB_EXIT_IDX0 0
#define TB_EXIT_IDX1 1
#define TB_EXIT_ICOUNT_EXPIRED 2
#define TB_EXIT_REQUESTED 3

#if !defined(tcg_qemu_tb_exec)
# define tcg_qemu_tb_exec(env, tb_ptr) \
    ((uintptr_t (*)(void *, void *))tcg_ctx->code_gen_prologue)(env, tb_ptr)
#endif

#if TCG_TARGET_MAYBE_vec
/* Return zero if the tuple (opc, type, vece) is unsupportable;
   return > 0 if it is directly supportable;
   return < 0 if we must call tcg_expand_vec_op.  */
int tcg_can_emit_vec_op(TCGOpcode, TCGType, unsigned);
#else
static inline int tcg_can_emit_vec_op(TCGOpcode o, TCGType t, unsigned ve)
{
    return 0;
}
#endif

/* Expand the tuple (opc, type, vece) on the given arguments.  */
void tcg_expand_vec_op(TCGContext *s, TCGOpcode, TCGType, unsigned, TCGArg, ...);

/* Replicate a constant C accoring to the log2 of the element size.  */
// Unicorn: renamed to avoid symbol clashing
uint64_t dup_const_impl(unsigned vece, uint64_t c);

#define dup_const(VECE, C)                                         \
    (__builtin_constant_p(VECE)                                    \
     ? (  (VECE) == MO_8  ? 0x0101010101010101ull * (uint8_t)(C)   \
        : (VECE) == MO_16 ? 0x0001000100010001ull * (uint16_t)(C)  \
        : (VECE) == MO_32 ? 0x0000000100000001ull * (uint32_t)(C)  \
        : dup_const_impl(VECE, C))                                      \
     : dup_const_impl(VECE, C))

/*
 * Memory helpers that will be used by TCG generated code.
 */
#ifdef CONFIG_SOFTMMU
/* Value zero-extended to tcg register size.  */
tcg_target_ulong helper_ret_ldub_mmu(CPUArchState *env, target_ulong addr,
                                     TCGMemOpIdx oi, uintptr_t retaddr);
tcg_target_ulong helper_le_lduw_mmu(CPUArchState *env, target_ulong addr,
                                    TCGMemOpIdx oi, uintptr_t retaddr);
tcg_target_ulong helper_le_ldul_mmu(CPUArchState *env, target_ulong addr,
                                    TCGMemOpIdx oi, uintptr_t retaddr);
uint64_t helper_le_ldq_mmu(CPUArchState *env, target_ulong addr,
                           TCGMemOpIdx oi, uintptr_t retaddr);
tcg_target_ulong helper_be_lduw_mmu(CPUArchState *env, target_ulong addr,
                                    TCGMemOpIdx oi, uintptr_t retaddr);
tcg_target_ulong helper_be_ldul_mmu(CPUArchState *env, target_ulong addr,
                                    TCGMemOpIdx oi, uintptr_t retaddr);
uint64_t helper_be_ldq_mmu(CPUArchState *env, target_ulong addr,
                           TCGMemOpIdx oi, uintptr_t retaddr);

/* Value sign-extended to tcg register size.  */
tcg_target_ulong helper_ret_ldsb_mmu(CPUArchState *env, target_ulong addr,
                                     TCGMemOpIdx oi, uintptr_t retaddr);
tcg_target_ulong helper_le_ldsw_mmu(CPUArchState *env, target_ulong addr,
                                    TCGMemOpIdx oi, uintptr_t retaddr);
tcg_target_ulong helper_le_ldsl_mmu(CPUArchState *env, target_ulong addr,
                                    TCGMemOpIdx oi, uintptr_t retaddr);
tcg_target_ulong helper_be_ldsw_mmu(CPUArchState *env, target_ulong addr,
                                    TCGMemOpIdx oi, uintptr_t retaddr);
tcg_target_ulong helper_be_ldsl_mmu(CPUArchState *env, target_ulong addr,
                                    TCGMemOpIdx oi, uintptr_t retaddr);

void helper_ret_stb_mmu(CPUArchState *env, target_ulong addr, uint8_t val,
                        TCGMemOpIdx oi, uintptr_t retaddr);
void helper_le_stw_mmu(CPUArchState *env, target_ulong addr, uint16_t val,
                       TCGMemOpIdx oi, uintptr_t retaddr);
void helper_le_stl_mmu(CPUArchState *env, target_ulong addr, uint32_t val,
                       TCGMemOpIdx oi, uintptr_t retaddr);
void helper_le_stq_mmu(CPUArchState *env, target_ulong addr, uint64_t val,
                       TCGMemOpIdx oi, uintptr_t retaddr);
void helper_be_stw_mmu(CPUArchState *env, target_ulong addr, uint16_t val,
                       TCGMemOpIdx oi, uintptr_t retaddr);
void helper_be_stl_mmu(CPUArchState *env, target_ulong addr, uint32_t val,
                       TCGMemOpIdx oi, uintptr_t retaddr);
void helper_be_stq_mmu(CPUArchState *env, target_ulong addr, uint64_t val,
                       TCGMemOpIdx oi, uintptr_t retaddr);

uint8_t helper_ret_ldb_cmmu(CPUArchState *env, target_ulong addr,
                            TCGMemOpIdx oi, uintptr_t retaddr);
uint16_t helper_le_ldw_cmmu(CPUArchState *env, target_ulong addr,
                            TCGMemOpIdx oi, uintptr_t retaddr);
uint32_t helper_le_ldl_cmmu(CPUArchState *env, target_ulong addr,
                            TCGMemOpIdx oi, uintptr_t retaddr);
uint64_t helper_le_ldq_cmmu(CPUArchState *env, target_ulong addr,
                            TCGMemOpIdx oi, uintptr_t retaddr);
uint16_t helper_be_ldw_cmmu(CPUArchState *env, target_ulong addr,
                            TCGMemOpIdx oi, uintptr_t retaddr);
uint32_t helper_be_ldl_cmmu(CPUArchState *env, target_ulong addr,
                            TCGMemOpIdx oi, uintptr_t retaddr);
uint64_t helper_be_ldq_cmmu(CPUArchState *env, target_ulong addr,
                            TCGMemOpIdx oi, uintptr_t retaddr);

uint32_t helper_atomic_cmpxchgb_mmu(CPUArchState *env, target_ulong addr,
                                    uint32_t cmpv, uint32_t newv,
                                    TCGMemOpIdx oi, uintptr_t retaddr);
uint32_t helper_atomic_cmpxchgw_le_mmu(CPUArchState *env, target_ulong addr,
                                       uint32_t cmpv, uint32_t newv,
                                       TCGMemOpIdx oi, uintptr_t retaddr);
uint32_t helper_atomic_cmpxchgl_le_mmu(CPUArchState *env, target_ulong addr,
                                       uint32_t cmpv, uint32_t newv,
                                       TCGMemOpIdx oi, uintptr_t retaddr);
uint64_t helper_atomic_cmpxchgq_le_mmu(CPUArchState *env, target_ulong addr,
                                       uint64_t cmpv, uint64_t newv,
                                       TCGMemOpIdx oi, uintptr_t retaddr);
uint32_t helper_atomic_cmpxchgw_be_mmu(CPUArchState *env, target_ulong addr,
                                       uint32_t cmpv, uint32_t newv,
                                       TCGMemOpIdx oi, uintptr_t retaddr);
uint32_t helper_atomic_cmpxchgl_be_mmu(CPUArchState *env, target_ulong addr,
                                       uint32_t cmpv, uint32_t newv,
                                       TCGMemOpIdx oi, uintptr_t retaddr);
uint64_t helper_atomic_cmpxchgq_be_mmu(CPUArchState *env, target_ulong addr,
                                       uint64_t cmpv, uint64_t newv,
                                       TCGMemOpIdx oi, uintptr_t retaddr);

#define GEN_ATOMIC_HELPER(NAME, TYPE, SUFFIX)         \
TYPE helper_atomic_ ## NAME ## SUFFIX ## _mmu         \
    (CPUArchState *env, target_ulong addr, TYPE val,  \
     TCGMemOpIdx oi, uintptr_t retaddr);

#define GEN_ATOMIC_HELPER_ALL(NAME)          \
    GEN_ATOMIC_HELPER(NAME, uint32_t, b)      \
    GEN_ATOMIC_HELPER(NAME, uint32_t, w_le)  \
    GEN_ATOMIC_HELPER(NAME, uint32_t, l_le)  \
    GEN_ATOMIC_HELPER(NAME, uint64_t, q_le)  \
    GEN_ATOMIC_HELPER(NAME, uint32_t, w_be)  \
    GEN_ATOMIC_HELPER(NAME, uint32_t, l_be)  \
    GEN_ATOMIC_HELPER(NAME, uint64_t, q_be)

GEN_ATOMIC_HELPER_ALL(fetch_add)
GEN_ATOMIC_HELPER_ALL(fetch_sub)
GEN_ATOMIC_HELPER_ALL(fetch_and)
GEN_ATOMIC_HELPER_ALL(fetch_or)
GEN_ATOMIC_HELPER_ALL(fetch_xor)

GEN_ATOMIC_HELPER_ALL(add_fetch)
GEN_ATOMIC_HELPER_ALL(sub_fetch)
GEN_ATOMIC_HELPER_ALL(and_fetch)
GEN_ATOMIC_HELPER_ALL(or_fetch)
GEN_ATOMIC_HELPER_ALL(xor_fetch)

GEN_ATOMIC_HELPER_ALL(xchg)

#undef GEN_ATOMIC_HELPER_ALL
#undef GEN_ATOMIC_HELPER

/* Temporary aliases until backends are converted.  */
#ifdef TARGET_WORDS_BIGENDIAN
# define helper_ret_ldsw_mmu  helper_be_ldsw_mmu
# define helper_ret_lduw_mmu  helper_be_lduw_mmu
# define helper_ret_ldsl_mmu  helper_be_ldsl_mmu
# define helper_ret_ldul_mmu  helper_be_ldul_mmu
# define helper_ret_ldl_mmu   helper_be_ldul_mmu
# define helper_ret_ldq_mmu   helper_be_ldq_mmu
# define helper_ret_stw_mmu   helper_be_stw_mmu
# define helper_ret_stl_mmu   helper_be_stl_mmu
# define helper_ret_stq_mmu   helper_be_stq_mmu
# define helper_ret_ldw_cmmu  helper_be_ldw_cmmu
# define helper_ret_ldl_cmmu  helper_be_ldl_cmmu
# define helper_ret_ldq_cmmu  helper_be_ldq_cmmu
#else
# define helper_ret_ldsw_mmu  helper_le_ldsw_mmu
# define helper_ret_lduw_mmu  helper_le_lduw_mmu
# define helper_ret_ldsl_mmu  helper_le_ldsl_mmu
# define helper_ret_ldul_mmu  helper_le_ldul_mmu
# define helper_ret_ldl_mmu   helper_le_ldul_mmu
# define helper_ret_ldq_mmu   helper_le_ldq_mmu
# define helper_ret_stw_mmu   helper_le_stw_mmu
# define helper_ret_stl_mmu   helper_le_stl_mmu
# define helper_ret_stq_mmu   helper_le_stq_mmu
# define helper_ret_ldw_cmmu  helper_le_ldw_cmmu
# define helper_ret_ldl_cmmu  helper_le_ldl_cmmu
# define helper_ret_ldq_cmmu  helper_le_ldq_cmmu
#endif

#endif /* CONFIG_SOFTMMU */

#ifdef CONFIG_ATOMIC128
#include "qemu/int128.h"

/* These aren't really a "proper" helpers because TCG cannot manage Int128.
   However, use the same format as the others, for use by the backends. */
Int128 helper_atomic_cmpxchgo_le_mmu(CPUArchState *env, target_ulong addr,
                                     Int128 cmpv, Int128 newv,
                                     TCGMemOpIdx oi, uintptr_t retaddr);
Int128 helper_atomic_cmpxchgo_be_mmu(CPUArchState *env, target_ulong addr,
                                     Int128 cmpv, Int128 newv,
                                     TCGMemOpIdx oi, uintptr_t retaddr);

Int128 helper_atomic_ldo_le_mmu(CPUArchState *env, target_ulong addr,
                                TCGMemOpIdx oi, uintptr_t retaddr);
Int128 helper_atomic_ldo_be_mmu(CPUArchState *env, target_ulong addr,
                                TCGMemOpIdx oi, uintptr_t retaddr);
void helper_atomic_sto_le_mmu(CPUArchState *env, target_ulong addr, Int128 val,
                              TCGMemOpIdx oi, uintptr_t retaddr);
void helper_atomic_sto_be_mmu(CPUArchState *env, target_ulong addr, Int128 val,
                              TCGMemOpIdx oi, uintptr_t retaddr);

#endif /* CONFIG_ATOMIC128 */

#endif /* TCG_H */
