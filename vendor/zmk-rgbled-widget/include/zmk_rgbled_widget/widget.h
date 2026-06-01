#define SHOW_LAYER_CHANGE                                                                          \
    (IS_ENABLED(CONFIG_RGBLED_WIDGET_SHOW_LAYER_CHANGE)) &&                                        \
        (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

#define SHOW_LAYER_COLORS                                                                          \
    (IS_ENABLED(CONFIG_RGBLED_WIDGET_SHOW_LAYER_COLORS)) &&                                        \
        (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

// Profiles only exist on the BLE central, so gate on CONFIG_ZMK_BLE as well
#define SHOW_PROFILE_COLORS                                                                         \
    (IS_ENABLED(CONFIG_RGBLED_WIDGET_SHOW_PROFILE_COLORS)) && (IS_ENABLED(CONFIG_ZMK_BLE)) &&       \
        (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))

// A split peripheral has no keymap; it shows layer color from the index the
// central pushes over CONFIG_ZMK_SPLIT_PERIPHERAL_LAYER_STATE.
#define SHOW_PERIPHERAL_LAYER_COLORS                                                                \
    (IS_ENABLED(CONFIG_RGBLED_WIDGET_SHOW_LAYER_COLORS)) && IS_ENABLED(CONFIG_ZMK_SPLIT) &&         \
        !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) &&                                               \
        IS_ENABLED(CONFIG_ZMK_SPLIT_PERIPHERAL_LAYER_STATE)

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
void indicate_battery(void);
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE)
void indicate_connectivity(void);
#endif

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
void indicate_layer(void);
#endif

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_SHOW_LAYER_COLORS)
/**
 * Set a layer's LED color at runtime and persist to settings.
 * @param layer_id  Layer index (0-based)
 * @param color_idx Color index 0-7 (Black=0, Red=1, Green=2, Yellow=3, Blue=4, Magenta=5, Cyan=6, White=7)
 */
void zmk_rgbled_widget_set_layer_color(uint8_t layer_id, uint8_t color_idx);
uint8_t zmk_rgbled_widget_get_layer_color(uint8_t layer_id);
#endif
