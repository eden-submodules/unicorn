/*
 * QEMU/MIPS pseudo-board
 *
 * emulates a simple machine with ISA-like bus.
 * ISA IO space mapped to the 0x14000000 (PHYS) and
 * ISA memory at the 0x10000000 (PHYS, 16Mb in size).
 * All peripherial devices are attached to this "bus" with
 * the standard PC ISA addresses.
*/

/* Unicorn Emulator Engine */
/* By Nguyen Anh Quynh, 2015 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "cpu.h"
#include "hw/hw.h"
#include "hw/mips/mips.h"
#include "hw/mips/cpudevs.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "exec/address-spaces.h"


static int mips_r4k_init(struct uc_struct *uc, MachineState *machine)
{
    const char *cpu_model = machine->cpu_model;

    /* init CPUs */
    if (cpu_model == NULL) {
#ifdef TARGET_MIPS64
        cpu_model = "R4000";
#else
        cpu_model = "24Kf";
#endif
    }

    uc->cpu = cpu_generic_init(uc, TYPE_MIPS_CPU, cpu_model);
    if (uc->cpu == NULL) {
        fprintf(stderr, "Unable to find CPU definition\n");
        return -1;
    }

    return 0;
}

static void mips_machine_init(struct uc_struct *uc, MachineClass *mc)
{
    mc->init = mips_r4k_init;
    mc->is_default = 1;
    mc->arch = UC_ARCH_MIPS;
}

DEFINE_MACHINE("mips", mips_machine_init)
