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
#include <zephyr/logging/log_ctrl.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define FATAL_MARK_MAGIC 0xFA7A1E55

static struct {
    uint32_t magic;
    uint32_t reason;
} fatal_mark __noinit;

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf) {
    ARG_UNUSED(esf);
    fatal_mark.magic = FATAL_MARK_MAGIC;
    fatal_mark.reason = reason;
    LOG_PANIC();
    LOG_ERR("Fatal error %u -> immediate reboot", reason);
    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}

uint8_t reset_diag_boot_color(void) {
    uint32_t cause = 0;

    if (hwinfo_get_reset_cause(&cause) == 0) {
        hwinfo_clear_reset_cause();
    }

    bool fatal = fatal_mark.magic == FATAL_MARK_MAGIC;
    uint32_t fatal_reason = fatal ? fatal_mark.reason : 0;
    fatal_mark.magic = 0;

    LOG_WRN("reset diag: cause=0x%08x fatal=%d reason=%u", cause, (int)fatal, fatal_reason);

    if (fatal) {
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
