// src/charge_indicator.c
//
// ZMK Feature: Charge Indicator (DT-driven, rgbled_adapter optional)
// - Reads charging status from a DT-defined chg_stat node (raw GPIO level: 0 = charging).
// - During charging, suppress widget output and either show a selected color or force LEDs OFF (Kconfig).
// - When not charging, keep LEDs OFF and let rgbled_widget handle all LED indications (no shortened durations).
// - Works with rgbled_adapter OR with custom DT that defines led-red/green/blue aliases; if aliases are absent, LED control is skipped safely.
// - Split-friendly: run the same code on both halves; each side reads and shows its own charging state.
// - No heap usage: uses a static thread to gently re-apply charging state only while charging.
//

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zmk/battery.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk_charge_indicator/charge_indicator.h>

LOG_MODULE_REGISTER(charge_indicator, CONFIG_ZMK_LOG_LEVEL);


/* Devicetree: Resolve charging status input from a chg_stat node via alias.
 * DT must provide:
 *   - a node: chg_stat: chg_stat { gpios = <&gpioX PIN GPIO_ACTIVE_LOW>; status = "okay"; };
 */
#define CHG_NODE        DT_NODELABEL(chg_stat)
#if !DT_NODE_HAS_STATUS(CHG_NODE, okay)
#error "DT alias chg-stat not found or not okay. Define aliases { chg-stat = &chg_stat; } and a chg_stat node."
#endif

#define CHG_CTLR        DT_GPIO_CTLR_BY_IDX(CHG_NODE, gpios, 0)
#define CHG_PIN_NUM     DT_GPIO_PIN_BY_IDX(CHG_NODE, gpios, 0)
/* Input flags:
 * - Use pull-up to keep the line high when PMIC STAT is open-drain (not charging).
 * - Do NOT set ACTIVE_LOW for input; we read raw level and treat 0 as charging consistently.
 */
#define CHG_PIN_FLAGS   (GPIO_INPUT | GPIO_PULL_UP)

/* Resolve RGB LED aliases, either provided by rgbled_adapter or your custom DT overlay.
 * If aliases are missing, we skip LED control entirely and let rgbled_widget (or others) use LEDs freely.
 */
#define LED_RED_ALIAS    DT_ALIAS(led_red)
#define LED_GREEN_ALIAS  DT_ALIAS(led_green)
#define LED_BLUE_ALIAS   DT_ALIAS(led_blue)

#if DT_NODE_HAS_STATUS(LED_RED_ALIAS, okay) && \
    DT_NODE_HAS_STATUS(LED_GREEN_ALIAS, okay) && \
    DT_NODE_HAS_STATUS(LED_BLUE_ALIAS, okay)
  /* Aliases present: enable LED control. */
  #define LEDR_CTLR   DT_GPIO_CTLR_BY_IDX(LED_RED_ALIAS, gpios, 0)
  #define LEDR_PIN    DT_GPIO_PIN_BY_IDX(LED_RED_ALIAS, gpios, 0)
  #define LEDR_FLAGS  (DT_GPIO_FLAGS_BY_IDX(LED_RED_ALIAS, gpios, 0) | GPIO_OUTPUT)

  #define LEDG_CTLR   DT_GPIO_CTLR_BY_IDX(LED_GREEN_ALIAS, gpios, 0)
  #define LEDG_PIN    DT_GPIO_PIN_BY_IDX(LED_GREEN_ALIAS, gpios, 0)
  #define LEDG_FLAGS  (DT_GPIO_FLAGS_BY_IDX(LED_GREEN_ALIAS, gpios, 0) | GPIO_OUTPUT)

  #define LEDB_CTLR   DT_GPIO_CTLR_BY_IDX(LED_BLUE_ALIAS, gpios, 0)
  #define LEDB_PIN    DT_GPIO_PIN_BY_IDX(LED_BLUE_ALIAS, gpios, 0)
  #define LEDB_FLAGS  (DT_GPIO_FLAGS_BY_IDX(LED_BLUE_ALIAS, gpios, 0) | GPIO_OUTPUT)
#else
  /* No LED aliases: disable LED control (always delegate to widget/other features). */
  #define CHARGE_INDICATOR_DISABLE_LED 1
#endif

/* State and devices */
static atomic_t is_charging = ATOMIC_INIT(false);

/* Battery level buckets (used by low/critical indicator). */
typedef enum {
    BAT_LVL_NORMAL = 0,
    BAT_LVL_LOW,
    BAT_LVL_CRITICAL,
} bat_level_t;

#if IS_ENABLED(CONFIG_CHG_LOW_BATTERY_INDICATOR)
static atomic_t bat_level = ATOMIC_INIT(BAT_LVL_NORMAL);
#endif
static const struct device *chg_dev;
#ifndef CHARGE_INDICATOR_DISABLE_LED
static const struct device *ledr_dev, *ledg_dev, *ledb_dev;
#endif
static struct gpio_callback chg_cb;

/* Debounce work for chg_handler — must be deferred from IRQ context (k_sleep
 * inside the GPIO callback is illegal in Zephyr and causes kernel panic /
 * silent halt, which is exactly the battery-only hang we've been chasing). */
static struct k_work_delayable chg_debounce_work;

/* Maintenance thread: reapply charging state periodically to suppress widget while charging. */
K_THREAD_STACK_DEFINE(chg_maint_stack, 512);
static struct k_thread chg_maint_thread;

/* Common-anode RGB (gpio-leds with GPIO_ACTIVE_LOW): write logical 1 to turn LED ON. */
#ifndef CHARGE_INDICATOR_DISABLE_LED
static inline void led_red(bool on)   { gpio_pin_set(ledr_dev, LEDR_PIN, on ? 1 : 0); }
static inline void led_green(bool on) { gpio_pin_set(ledg_dev, LEDG_PIN, on ? 1 : 0); }
static inline void led_blue(bool on)  { gpio_pin_set(ledb_dev, LEDB_PIN, on ? 1 : 0); }

/* Apply LED color based on color code (0-7). */
static inline void apply_color_code(int color)
{
    LOG_DBG("Applying color code: %d", color);
    switch (color) {
        case 0: /* Black(off) */             led_red(false); led_green(false); led_blue(false); break;
        case 1: /* Red */                    led_red(true);  led_green(false); led_blue(false); break;
        case 2: /* Green */                  led_red(false); led_green(true);  led_blue(false); break;
        case 3: /* Yellow(R+G) */            led_red(true);  led_green(true);  led_blue(false); break;
        case 4: /* Blue */                   led_red(false); led_green(false); led_blue(true);  break;
        case 5: /* Magenta(R+B) */           led_red(true);  led_green(false); led_blue(true);  break;
        case 6: /* Cyan(G+B) */              led_red(false); led_green(true);  led_blue(true);  break;
        case 7: /* White(R+G+B) */           led_red(true);  led_green(true);  led_blue(true);  break;
        default: /* Fallback Red */          led_red(true);  led_green(false); led_blue(false); break;
    }
}

/* Get battery level based color code. */
#if IS_ENABLED(CONFIG_CHG_BATTERY_LEVEL_BASED_COLOR)
static int get_battery_level_color(void)
{
    uint8_t battery_pct = zmk_battery_state_of_charge();
    LOG_DBG("Battery level: %d%%", battery_pct);

    if (battery_pct < 0 || battery_pct > 100) {
        return CONFIG_CHG_BATTERY_COLOR_MISSING;
    }

    if (battery_pct < CONFIG_CHG_BATTERY_LEVEL_CRITICAL) {
        return CONFIG_CHG_BATTERY_COLOR_CRITICAL;
    } else if (battery_pct < CONFIG_CHG_BATTERY_LEVEL_LOW) {
        return CONFIG_CHG_BATTERY_COLOR_LOW;
    } else if (battery_pct < CONFIG_CHG_BATTERY_LEVEL_HIGH) {
        return CONFIG_CHG_BATTERY_COLOR_MEDIUM;
    } else {
        return CONFIG_CHG_BATTERY_COLOR_HIGH;
    }
}
#endif
#endif

/* Classify current ZMK battery percentage into NORMAL/LOW/CRITICAL.
 * Defined outside the LED-enabled block: it tracks state only and is safe
 * to call even when LED control is unavailable. */
#if IS_ENABLED(CONFIG_CHG_LOW_BATTERY_INDICATOR)
static void update_low_battery(void)
{
    uint8_t pct = zmk_battery_state_of_charge();
    /* pct == 0 may indicate "no reading yet" — treat as NORMAL to avoid
     * false-positive low/critical at boot before the first ADC sample. */
    bat_level_t level = BAT_LVL_NORMAL;
    if (pct > 0) {
        if (pct <= CONFIG_CHG_CRITICAL_BATTERY_THRESHOLD) {
            level = BAT_LVL_CRITICAL;
        } else if (pct <= CONFIG_CHG_LOW_BATTERY_THRESHOLD) {
            level = BAT_LVL_LOW;
        }
    }
    atomic_set(&bat_level, (atomic_val_t)level);
    LOG_DBG("Battery: pct=%d%% level=%d", pct, (int)level);
}
#endif

/* Read raw physical level:
 * - 0 = charging (STAT active low, PMIC drives low)
 * - 1 = not charging (open-drain released; internal pull-up keeps high)
 */
static inline bool read_charging(void) {
    int raw = gpio_pin_get_raw(chg_dev, CHG_PIN_NUM);
    return raw == 0;
}

/* Apply final LED color based on current charging / low-battery state.
 * Priority: charging > critical (blinking, owned by maint thread) >
 *           low (solid) > delegate-to-widget (LED off). */
static void apply_state(void)
{
#ifndef CHARGE_INDICATOR_DISABLE_LED
    if (atomic_get(&is_charging)) {
#if IS_ENABLED(CONFIG_CHG_POLICY)
        /* Charging: force LEDs OFF, fully suppress widget output. */
        led_red(false); led_green(false); led_blue(false);
#else
        /* Charging: show color based on configuration. */
#if IS_ENABLED(CONFIG_CHG_BATTERY_LEVEL_BASED_COLOR)
        apply_color_code(get_battery_level_color());
#else
        apply_color_code(CONFIG_CHG_COLOR);
#endif
#endif
        return;
    }

#if IS_ENABLED(CONFIG_CHG_LOW_BATTERY_INDICATOR)
    bat_level_t lvl = (bat_level_t)atomic_get(&bat_level);
    if (lvl == BAT_LVL_CRITICAL) {
        /* Critical: blinking is driven by the maintenance thread.
         * Don't override here — IRQ/listener calls must not stomp on the phase. */
        return;
    }
    if (lvl == BAT_LVL_LOW) {
        /* Low (not critical, not charging): solid color, suppress widget output. */
        apply_color_code(CONFIG_CHG_LOW_BATTERY_COLOR);
        return;
    }
#endif

    /* Idle: keep LEDs OFF and fully delegate to rgbled_widget/others. */
    led_red(false); led_green(false); led_blue(false);
#endif
}

/* Debounce work handler: runs in system workqueue (NOT IRQ context),
 * safe to do GPIO reads and state updates here. */
static void chg_debounce_work_handler(struct k_work *work)
{
    bool charging = read_charging();
    atomic_set(&is_charging, charging);
    apply_state();
}

/* IRQ handler: defer everything to a delayable work to leave IRQ context.
 * (Previously this called k_sleep(K_MSEC(8)) inside the IRQ callback, which
 * is illegal in Zephyr — caused __ASSERT failure / kernel panic / silent halt
 * once a STAT pin edge fired on battery operation. That was the root cause of
 * the 30~60min hang reproduced across all v0.4.x builds.) */
static void chg_handler(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
    /* Defer to system workqueue with 8ms debounce — runs in thread context. */
    k_work_reschedule(&chg_debounce_work, K_MSEC(8));
}

/* Battery state changed event handler: refresh low-battery flag and reapply. */
static int battery_state_changed_listener(const zmk_event_t *eh)
{
    const struct zmk_battery_state_changed *ev = as_zmk_battery_state_changed(eh);
    if (ev == NULL) {
        return -ENOTSUP;
    }

#if IS_ENABLED(CONFIG_CHG_LOW_BATTERY_INDICATOR)
    update_low_battery();
#endif
    apply_state();

    return 0;
}

ZMK_LISTENER(charge_indicator, battery_state_changed_listener);
ZMK_SUBSCRIPTION(charge_indicator, zmk_battery_state_changed);

/* Maintenance thread:
 * - Charging or low battery (solid): periodically reapply to suppress widget
 *   (prevent short blinks from stealing the LED).
 * - Critical battery: drive the blink directly here (alternate color/off).
 * - Otherwise: sleep and do nothing (preserve widget timing completely).
 */
static void charging_maint_task(void)
{
    bool blink_phase = false;
    while (true) {
        bool charging = atomic_get(&is_charging);
        bat_level_t lvl = BAT_LVL_NORMAL;
#if IS_ENABLED(CONFIG_CHG_LOW_BATTERY_INDICATOR)
        lvl = (bat_level_t)atomic_get(&bat_level);
#endif

        if (!charging && lvl == BAT_LVL_CRITICAL) {
#ifndef CHARGE_INDICATOR_DISABLE_LED
            blink_phase = !blink_phase;
            if (blink_phase) {
                apply_color_code(CONFIG_CHG_CRITICAL_BATTERY_COLOR);
            } else {
                led_red(false); led_green(false); led_blue(false);
            }
#endif
            k_sleep(K_MSEC(CONFIG_CHG_CRITICAL_BLINK_HALF_PERIOD_MS));
        } else if (charging || lvl == BAT_LVL_LOW) {
            blink_phase = false;
            apply_state();
            k_sleep(K_MSEC(150)); /* Tune for stronger/weaker suppression vs. power. */
        } else {
            blink_phase = false;
            k_sleep(K_SECONDS(1));
        }
    }
}

/* Initialization:
 * - Resolve DT devices
 * - Configure pins
 * - Stabilization wait + double-read debounce for initial state
 * - IRQ setup
 * - Start maintenance thread
 */
static int charge_indicator_init(void)
{
    if (!IS_ENABLED(CONFIG_CHARGE_INDICATOR)) {
        LOG_INF("Charge indicator disabled by Kconfig");
        return 0;
    }

    /* Input (STAT) controller device */
    chg_dev  = DEVICE_DT_GET(CHG_CTLR);
    if (!device_is_ready(chg_dev)) {
        LOG_ERR("CHG GPIO controller not ready");
        return -ENODEV;
    }

#ifndef CHARGE_INDICATOR_DISABLE_LED
    /* LED controllers */
    ledr_dev = DEVICE_DT_GET(LEDR_CTLR);
    ledg_dev = DEVICE_DT_GET(LEDG_CTLR);
    ledb_dev = DEVICE_DT_GET(LEDB_CTLR);
    if (!device_is_ready(ledr_dev) || !device_is_ready(ledg_dev) || !device_is_ready(ledb_dev)) {
        LOG_ERR("LED GPIO controller not ready");
        return -ENODEV;
    }
#endif

    /* Configure STAT input with pull-up (raw read will be used). */
    int ret = gpio_pin_configure(chg_dev, CHG_PIN_NUM, CHG_PIN_FLAGS);
    if (ret) { LOG_ERR("CHG pin cfg failed: %d", ret); return ret; }

#ifndef CHARGE_INDICATOR_DISABLE_LED
    /* Configure RGB output pins. */
    ret = gpio_pin_configure(ledr_dev, LEDR_PIN, LEDR_FLAGS);
    if (ret) { LOG_ERR("LEDR cfg failed: %d", ret); return ret; }
    ret = gpio_pin_configure(ledg_dev, LEDG_PIN, LEDG_FLAGS);
    if (ret) { LOG_ERR("LEDG cfg failed: %d", ret); return ret; }
    ret = gpio_pin_configure(ledb_dev, LEDB_PIN, LEDB_FLAGS);
    if (ret) { LOG_ERR("LEDB cfg failed: %d", ret); return ret; }
#endif

    /* Initial stabilization + double-read debounce. */
    k_sleep(K_MSEC(20));
    bool c1 = read_charging();
    k_sleep(K_MSEC(10));
    bool c2 = read_charging();
    bool charging_init = (c1 && c2);
    atomic_set(&is_charging, charging_init);
#if IS_ENABLED(CONFIG_CHG_LOW_BATTERY_INDICATOR)
    update_low_battery();
#endif
    apply_state();

    /* Initialize debounce work for IRQ context offloading. */
    k_work_init_delayable(&chg_debounce_work, chg_debounce_work_handler);

    /* IRQ on both edges. */
    ret = gpio_pin_interrupt_configure(chg_dev, CHG_PIN_NUM, GPIO_INT_EDGE_BOTH);
    if (ret) { LOG_ERR("CHG int cfg failed: %d", ret); return ret; }

    gpio_init_callback(&chg_cb, chg_handler, BIT(CHG_PIN_NUM));
    ret = gpio_add_callback(chg_dev, &chg_cb);
    if (ret) { LOG_ERR("CHG add cb failed: %d", ret); return ret; }

    /* Start maintenance thread (charging-only suppression). */
    k_tid_t tid = k_thread_create(&chg_maint_thread,
                                  chg_maint_stack, K_THREAD_STACK_SIZEOF(chg_maint_stack),
                                  (k_thread_entry_t)charging_maint_task,
                                  NULL, NULL, NULL,
                                  K_LOWEST_THREAD_PRIO, 0, K_NO_WAIT);
    k_thread_name_set(tid, "chg_maint");

    LOG_INF("Charge indicator init: pin=%d, charging=%d, tid=%p", CHG_PIN_NUM, charging_init, tid);
    return 0;
}

/* Public API: query charging state from other modules (e.g., rgbled_widget). */
bool zmk_charge_indicator_is_charging(void)
{
#if IS_ENABLED(CONFIG_CHARGE_INDICATOR)
    return (bool)atomic_get(&is_charging);
#else
    return false;
#endif
}

/* Run after widgets to make suppression predictable. */
SYS_INIT(charge_indicator_init, APPLICATION, 70);
