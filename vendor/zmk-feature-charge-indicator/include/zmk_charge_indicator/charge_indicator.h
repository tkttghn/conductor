// include/zmk_charge_indicator/charge_indicator.h
//
// Public API for zmk-feature-charge-indicator.
// Provides a way for other modules (e.g., zmk-rgbled-widget) to query
// the current charging state without direct GPIO access.

#pragma once

#include <stdbool.h>

/**
 * @brief Check if the keyboard is currently charging.
 *
 * Returns true if the PMIC STAT pin indicates active charging.
 * Thread-safe (uses atomic read).
 *
 * If CONFIG_CHARGE_INDICATOR is not enabled, always returns false.
 */
bool zmk_charge_indicator_is_charging(void);
