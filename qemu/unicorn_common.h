#ifndef UNICORN_COMMON_H_
#define UNICORN_COMMON_H_

#include "tcg.h"

// This header define common patterns/codes that will be included in all arch-sepcific
// codes for unicorns purposes.

// return true on success, false on failure
static inline bool cpu_physical_mem_read(AddressSpace *as, hwaddr addr,
                                            uint8_t *buf, int len)
{
    return !cpu_physical_memory_rw(as, addr, (void *)buf, len, 0);
}

static inline bool cpu_physical_mem_write(AddressSpace *as, hwaddr addr,
                                            const uint8_t *buf, int len)
{
    return !cpu_physical_memory_rw(as, addr, (void *)buf, len, 1);
}

void tb_cleanup(struct uc_struct *uc);
void free_code_gen_buffer(struct uc_struct *uc);

static inline void free_address_spaces(struct uc_struct *uc)
{
    int i;

    address_space_destroy(&uc->as);
    for (i = 0; i < uc->cpu->num_ases; i++) {
        AddressSpace *as = uc->cpu->cpu_ases[i].as;
        address_space_destroy(as);
        g_free(as);
    }
}

/* This is *supposed* to be done by the class finalizer but it never executes */
static inline void free_machine_class_name(struct uc_struct *uc) {
    MachineClass *mc = MACHINE_GET_CLASS(uc, uc->machine_state);

    g_free(mc->name);
    mc->name = NULL;
}

static inline void free_tcg_temp_names(TCGContext *s)
{
#if TCG_TARGET_REG_BITS == 32
    int i;

    for (i = 0; i < s->nb_globals; i++) {
        TCGTemp *ts = &s->temps[i];
        if (ts->base_type == TCG_TYPE_I64) {
            if (ts->name && ((strcmp(ts->name+(strlen(ts->name)-2), "_0") == 0) ||
                        (strcmp(ts->name+(strlen(ts->name)-2), "_1") == 0))) {
                free((void *)ts->name);
            }
        }
    }
#endif
}

static inline void free_tcg_context(TCGContext *s)
{
    TCGOpDef* def = &s->tcg_op_defs[0];
    TCGPool *po, *to;

    g_free(def->args_ct);
    g_free(def->sorted_args);
    g_free(s->tcg_op_defs);

    for (po = s->pool_first; po; po = to) {
        to = po->next;
        g_free(po);
    }
    tcg_pool_reset(s);

    g_hash_table_destroy(s->helpers);
    free_tcg_temp_names(s);
    g_free(s);
}

static inline void free_tcg_contexts(struct uc_struct *uc)
{
    int i;
    TCGContext **tcg_ctxs = uc->tcg_ctxs;

    for (i = 0; i < uc->n_tcg_ctxs; i++) {
        free_tcg_context(tcg_ctxs[i]);
    }

    g_free(tcg_ctxs);
}

/** Freeing common resources */
static void release_common(void *t)
{
    TCGContext *s = (TCGContext *)t;
    struct uc_struct *uc = s->uc;

    // Clean TCG.
    free_tcg_contexts(uc);
    g_tree_destroy(uc->tb_ctx.tb_tree);
    qht_destroy(&uc->tb_ctx.htable);

    // Destory flat view hash table
    g_hash_table_destroy(uc->flat_views);
    unicorn_free_empty_flat_view(uc);

    // TODO(danghvu): these function is not available outside qemu
    // so we keep them here instead of outside uc_close.
    free_address_spaces(uc);
    memory_free(uc);
    tb_cleanup(uc);
    free_code_gen_buffer(uc);
    free_machine_class_name(uc);
}

static inline void uc_common_init(struct uc_struct* uc)
{
    memory_register_types(uc);
    uc->write_mem = cpu_physical_mem_write;
    uc->read_mem = cpu_physical_mem_read;
    uc->tcg_enabled = tcg_enabled;
    uc->tcg_exec_init = tcg_exec_init;
    uc->cpu_exec_init_all = cpu_exec_init_all;
    uc->vm_start = vm_start;
    uc->memory_map = memory_map;
    uc->memory_map_ptr = memory_map_ptr;
    uc->memory_unmap = memory_unmap;
    uc->readonly_mem = memory_region_set_readonly;

    uc->target_page_size = TARGET_PAGE_SIZE;
    uc->target_page_align = TARGET_PAGE_SIZE - 1;

    if (!uc->release) {
        uc->release = release_common;
    }
}

#endif
