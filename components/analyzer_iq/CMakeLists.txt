# SPDX-License-Identifier: GPL-3.0-or-later
# Raw I/Q capture (ESP-SDR RF dump, original ESP32). Pure C component: the
# app component is C++, and its flags must not reach these sources.
set(esp_sdr_dir "${CMAKE_CURRENT_LIST_DIR}/../../firmware/esp-sdr/main")

idf_component_register(
    SRCS
        "iq_radio.c"
        "${esp_sdr_dir}/common/rx_recalibration.c"
        "${esp_sdr_dir}/common/rx_calibration_state.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "${esp_sdr_dir}/common"
    PRIV_REQUIRES esp_phy esp_rom esp_timer esp_wifi esp_hw_support heap soc
)

# Dump bank 0x3ffe8000..0x3fff8000: keep static data out of it, and hook the
# channel analog setup for off-channel DC calibration (rx_recalibration.c).
target_link_options(${COMPONENT_LIB} INTERFACE
    "-T${esp_sdr_dir}/targets/esp32/sram_guard.ld"
    "-Wl,--wrap=chip_v7_set_chan_ana")
# SOC_RESERVE_MEMORY_REGION lives in this archive; keep it linked.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,-u,iq_radio_capture")
