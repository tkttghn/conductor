cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(zmk_feature_charge_indicator)

if(CONFIG_CHARGE_INDICATOR)
  target_sources(app PRIVATE src/charge_indicator.c)
endif()

# Always expose the public header so other modules can include it
# (zmk_charge_indicator_is_charging returns false when disabled)
target_include_directories(app PUBLIC include)