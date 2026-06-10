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

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fatal.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define FATAL_MARK_MAGIC 0xFA7A1E55

static struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc;
    uint32_t lr;
} fatal_mark __noinit;

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    fatal_mark.magic = FATAL_MARK_MAGIC;
    fatal_mark.reason = reason;
    fatal_mark.pc = esf ? esf->basic.pc : 0;
    fatal_mark.lr = esf ? esf->basic.lr : 0;
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

static void diag_report_cb(struct k_work *work) {
    LOG_WRN("reset diag: cause=0x%08x fatal=%d reason=%u pc=0x%08x lr=0x%08x", last_cause,
            (int)last_fatal, last_reason, last_pc, last_lr);
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
    fatal_mark.magic = 0;

    LOG_WRN("reset diag: cause=0x%08x fatal=%d reason=%u pc=0x%08x lr=0x%08x", last_cause,
            (int)last_fatal, last_reason, last_pc, last_lr);

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
