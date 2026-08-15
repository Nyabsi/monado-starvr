// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ITE firmware bridge for the StarVR One.
 * @author Nyabsi <nyabsi@sovellus.cc>
 * @ingroup drv_starvr
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_prober_device;

//! The bridge chip owns the panel, the EEPROM and the front facing status.
struct starvr_hid;

struct starvr_hid *
starvr_hid_open(struct xrt_prober_device *xpdev);

void
starvr_hid_close(struct starvr_hid *hid);

//! Byte offset into the 32 KiB EEPROM, the transfer is chunked internally.
bool
starvr_hid_eeprom_read(struct starvr_hid *hid, uint16_t address, uint16_t length, void *out_buffer);

//! An index into @ref starvr_panel_rates rather than a rate.
bool
starvr_hid_fps_setting_read(struct starvr_hid *hid, uint8_t *out_setting);

//! Analog gain of the panel, one value per eye, 0 to 255.
bool
starvr_hid_brightness_read(struct starvr_hid *hid, uint8_t out_values[2]);

bool
starvr_hid_brightness_write(struct starvr_hid *hid, uint8_t channel, uint8_t value);

//! Are the panels currently lit, this is what the proximity sensor drives.
bool
starvr_hid_display_on_read(struct starvr_hid *hid, bool *out_on);

bool
starvr_hid_fw_version_read(struct starvr_hid *hid, char out_version[33]);

#ifdef __cplusplus
}
#endif
