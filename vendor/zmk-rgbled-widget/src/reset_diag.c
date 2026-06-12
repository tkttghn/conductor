/*
 * Reset-reason diagnostics for the multi-host reboot investigation.
 *
 * On boot, read (and clear) the hardware reset cause plus a __noinit fatal
 * marker, log them, and hand the LED widget a blink color:
 *   yellow  = fatal error path (BT_ASSERT / k_oops / k_panic / CPU fault)
 *   red     = hardware watchdog without a fatal marker (feed starved)
 *   magenta = CPU lockup
 *   cyan    = other software reset (incl. leaving the UF2 bootloader)
 *   none    = power-on / reset pin
 *
 * Also overrides the weak k_sys_fatal_error_handler: the default halts,
 * which would only reboot ~30s later through the watchdog and report DOG
 * as the reset cause, masking the real origin. Mark the reason in __noinit
 * RAM (survives a warm reset) and reboot right away instead.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <cmsis_core.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define FATAL_MARK_MAGIC 0xFA7A1E55

/* Prepare-pipeline snapshot written by the zephyr diag fork (lll.c) right
 * before the controller asserts on a full pipeline. The weak zero instance
 * keeps the link intact on builds without that fork patch. Layout must
 * match lll.c's definition. */
#define LLL_DIAG_PIPELINE_MAGIC   0x11D1A65C
#define LLL_DIAG_PIPELINE_ENT_MAX 16

struct lll_diag_pipeline_ent {
    uint32_t prepare_cb;
    uint32_t param;
    uint8_t is_resume;
    uint8_t is_aborted;
};

struct lll_diag_pipeline_snap {
    uint32_t magic;
    uint32_t count;
    struct lll_diag_pipeline_ent ent[LLL_DIAG_PIPELINE_ENT_MAX];
};

__weak struct lll_diag_pipeline_snap lll_diag_pipeline_snap;

static struct lll_diag_pipeline_snap last_pipeline;

static void diag_log_pipeline(void) {
    if (last_pipeline.magic != LLL_DIAG_PIPELINE_MAGIC) {
        return;
    }
    for (uint32_t i = 0; i < last_pipeline.count && i < LLL_DIAG_PIPELINE_ENT_MAX; i++) {
        LOG_WRN("pipeline[%02u]: cb=0x%08x param=0x%08x resume=%u aborted=%u", i,
                last_pipeline.ent[i].prepare_cb, last_pipeline.ent[i].param,
                last_pipeline.ent[i].is_resume, last_pipeline.ent[i].is_aborted);
    }
}

static struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc;
    uint32_t lr;
    char thread[16];
    /* ARM fault details: which address was touched (MMFAR/BFAR) tells WHOSE
     * stack guard was hit — the _current thread may just be the victim of an
     * ISR/foreign stack overflow, not the owner of the overflowed stack. */
    uint32_t cfsr;
    uint32_t mmfar;
    uint32_t bfar;
    int32_t stack_unused;
} fatal_mark __noinit;

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    fatal_mark.magic = FATAL_MARK_MAGIC;
    fatal_mark.reason = reason;
    fatal_mark.pc = esf ? esf->basic.pc : 0;
    fatal_mark.lr = esf ? esf->basic.lr : 0;
    fatal_mark.cfsr = SCB->CFSR;
    fatal_mark.mmfar = SCB->MMFAR;
    fatal_mark.bfar = SCB->BFAR;
    /* Which thread died? Decisive for stack overflows, where the esf is
     * garbage. Needs CONFIG_THREAD_NAME (k_thread_name_get returns NULL
     * otherwise, which is fine). */
    fatal_mark.thread[0] = '\0';
    fatal_mark.stack_unused = -1;
    struct k_thread *th = k_sched_current_thread_query();
    if (th) {
        const char *name = k_thread_name_get(th);
        if (name) {
            strncpy(fatal_mark.thread, name, sizeof(fatal_mark.thread) - 1);
            fatal_mark.thread[sizeof(fatal_mark.thread) - 1] = '\0';
        }
        /* Unused headroom of the current thread's stack (CONFIG_INIT_STACKS):
         * ~0 => this thread's own stack really overflowed; large => the fault
         * came from somewhere else (ISR stack, wild pointer). */
#if IS_ENABLED(CONFIG_INIT_STACKS) && IS_ENABLED(CONFIG_THREAD_STACK_INFO)
        size_t unused = 0;
        if (k_thread_stack_space_get(th, &unused) == 0) {
            fatal_mark.stack_unused = (int32_t)unused;
        }
#endif
    }
    /* No LOG_PANIC() here: flushing to the USB CDC log backend spins forever
     * when no host is draining the port, and the watchdog fires before
     * sys_reboot() runs (seen live: fatal mark set but RESETREAS showed DOG,
     * not SREQ). The __noinit mark carries everything; reboot immediately. */
    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}

/* Last boot's diagnosis, re-logged every 60s so the fatal details can be
 * read over USB logging at any time after the reboot, not just at boot. */
static uint32_t last_cause;
static bool last_fatal;
static uint32_t last_reason;
static uint32_t last_pc;
static uint32_t last_lr;
static char last_thread[16];
static uint32_t last_cfsr;
static uint32_t last_mmfar;
static uint32_t last_bfar;
static int32_t last_stack_unused;

static void diag_report_cb(struct k_work *work) {
    LOG_WRN("reset diag: cause=0x%08x fatal=%d reason=%u pc=0x%08x lr=0x%08x thread=%s "
            "cfsr=0x%08x mmfar=0x%08x bfar=0x%08x unused=%d",
            last_cause, (int)last_fatal, last_reason, last_pc, last_lr, last_thread, last_cfsr,
            last_mmfar, last_bfar, last_stack_unused);
    diag_log_pipeline();
    k_work_schedule(k_work_delayable_from_work(work), K_SECONDS(60));
}

static K_WORK_DELAYABLE_DEFINE(diag_report_work, diag_report_cb);

uint8_t reset_diag_boot_color(void) {
    uint32_t cause = 0;

    if (hwinfo_get_reset_cause(&cause) == 0) {
        hwinfo_clear_reset_cause();
    }

    last_cause = cause;
    last_fatal = fatal_mark.magic == FATAL_MARK_MAGIC;
    last_reason = last_fatal ? fatal_mark.reason : 0;
    last_pc = last_fatal ? fatal_mark.pc : 0;
    last_lr = last_fatal ? fatal_mark.lr : 0;
    last_thread[0] = '\0';
    last_cfsr = last_fatal ? fatal_mark.cfsr : 0;
    last_mmfar = last_fatal ? fatal_mark.mmfar : 0;
    last_bfar = last_fatal ? fatal_mark.bfar : 0;
    last_stack_unused = last_fatal ? fatal_mark.stack_unused : -1;
    if (last_fatal) {
        memcpy(last_thread, fatal_mark.thread, sizeof(last_thread));
        last_thread[sizeof(last_thread) - 1] = '\0';
    }
    fatal_mark.magic = 0;

    if (lll_diag_pipeline_snap.magic == LLL_DIAG_PIPELINE_MAGIC) {
        memcpy(&last_pipeline, &lll_diag_pipeline_snap, sizeof(last_pipeline));
        lll_diag_pipeline_snap.magic = 0;
    }

    LOG_WRN("reset diag: cause=0x%08x fatal=%d reason=%u pc=0x%08x lr=0x%08x thread=%s "
            "cfsr=0x%08x mmfar=0x%08x bfar=0x%08x unused=%d",
            last_cause, (int)last_fatal, last_reason, last_pc, last_lr, last_thread, last_cfsr,
            last_mmfar, last_bfar, last_stack_unused);
    diag_log_pipeline();

    if (last_fatal || (cause & (RESET_WATCHDOG | RESET_CPU_LOCKUP))) {
        k_work_schedule(&diag_report_work, K_SECONDS(60));
    }

    if (last_fatal) {
        return 3; /* red+green = yellow */
    }
    if (cause & RESET_WATCHDOG) {
        return 1; /* red */
    }
    if (cause & RESET_CPU_LOCKUP) {
        return 5; /* red+blue = magenta */
    }
    if (cause & RESET_SOFTWARE) {
        return 6; /* green+blue = cyan */
    }
    return 0;
}
