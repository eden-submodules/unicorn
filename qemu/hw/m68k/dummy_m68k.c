/*
 * Dummy board with just RAM and CPU for use as an ISS.
 *
 * Copyright (c) 2007 CodeSourcery.
 *
 * This code is licensed under the GPL
 */

/* Unicorn Emulator Engine */
/* By Nguyen Anh Quynh, 2015 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "hw/hw.h"
#include "hw/m68k/m68k.h"
#include "hw/boards.h"
#include "exec/address-spaces.h"

/* Board init.  */
static int dummy_m68k_init(struct uc_struct *uc, MachineState *machine)
{
    const char *cpu_model = machine->cpu_model;
    CPUM68KState *env;

    if (!cpu_model)
        cpu_model = "cfv4e";

    env = cpu_init(uc, cpu_model);
    if (!env) {
        fprintf(stderr, "Unable to find m68k CPU definition\n");
        return -1;
    }

    /* Initialize CPU registers.  */
    env->vbr = 0;
    env->pc = 0;

    return 0;
}

static void dummy_m68k_machine_init(struct uc_struct *uc, MachineClass *mc)
{
    mc->init = dummy_m68k_init;
    mc->is_default = 1;
    mc->arch = UC_ARCH_M68K;
}

DEFINE_MACHINE("dummy", dummy_m68k_machine_init)
