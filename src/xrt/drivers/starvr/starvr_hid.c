// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  ITE firmware bridge for the StarVR One.
 * @author Nyabsi <nyabsi@sovellus.cc>
 * @ingroup drv_starvr
 */

#include "starvr_hid.h"
#include "starvr_protocol.h"

#include "xrt/xrt_prober.h"

#include "util/u_logging.h"
#include "util/u_misc.h"

#include <string.h>
#include <libusb.h>

static const uint8_t starvr_command_opcodes[] = {
    0x00, 0x01, 0x02, 0x10, 0x11, 0x20, 0x30, 0x06,
    0x03, 0x04, 0x05, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x40, 0x41, 0x12,
};

struct starvr_hid
{
	libusb_context *ctx;
	libusb_device_handle *dev;

	bool claimed;
	bool detached;
};

static bool
transfer_out(struct starvr_hid *hid, const void *data)
{
	int transferred = 0;
	int ret = libusb_interrupt_transfer(hid->dev, STARVR_EP_OUT, (unsigned char *)data, STARVR_XFER_SIZE,
	                                    &transferred, STARVR_USB_TIMEOUT_MS);

	if (ret < 0 || transferred != STARVR_XFER_SIZE) {
		U_LOG_E("StarVR: write failed, %s", libusb_error_name(ret));
		return false;
	}

	return true;
}

static bool
transfer_in(struct starvr_hid *hid, void *data)
{
	int transferred = 0;
	int ret = libusb_interrupt_transfer(hid->dev, STARVR_EP_IN, (unsigned char *)data, STARVR_XFER_SIZE,
	                                    &transferred, STARVR_USB_TIMEOUT_MS);

	if (ret < 0 || transferred != STARVR_XFER_SIZE) {
		U_LOG_E("StarVR: read failed, %s", libusb_error_name(ret));
		return false;
	}

	return true;
}

/*!
 * The bridge occasionally answers a stale request, so the reply is matched
 * against what was sent and asked for again if it does not line up.
 */
static bool
send_command(struct starvr_hid *hid,
             enum starvr_command_index command,
             uint8_t payload_length,
             const void *payload,
             struct starvr_ctrl_pkt *out_reply)
{
	if (hid == NULL || command >= ARRAY_SIZE(starvr_command_opcodes)) {
		return false;
	}

	const uint8_t opcode = starvr_command_opcodes[command];

	for (int attempt = 0; attempt < STARVR_RETRY_COUNT; attempt++) {
		struct starvr_ctrl_pkt pkt = {0};
		pkt.packet = STARVR_PACKET_COMMAND_REQUEST;
		pkt.opcode = opcode;

		if (payload_length > 0 && payload != NULL) {
			if (payload_length > sizeof(pkt.data)) {
				return false;
			}
			memcpy(pkt.data, payload, payload_length);
		}

		if (!transfer_out(hid, &pkt)) {
			return false;
		}

		memset(&pkt, 0, sizeof(pkt));

		if (!transfer_in(hid, &pkt)) {
			return false;
		}

		if (pkt.packet != STARVR_PACKET_COMMAND_REPLY || pkt.opcode != opcode) {
			continue;
		}

		if (pkt.status != 0) {
			U_LOG_E("StarVR: bridge refused opcode 0x%02x", opcode);
			return false;
		}

		if (out_reply != NULL) {
			*out_reply = pkt;
		}

		return true;
	}

	U_LOG_E("StarVR: no reply for opcode 0x%02x", opcode);

	return false;
}

struct starvr_hid *
starvr_hid_open(struct xrt_prober_device *xpdev)
{
	if (xpdev == NULL) {
		return NULL;
	}

	struct starvr_hid *hid = U_TYPED_CALLOC(struct starvr_hid);
	if (hid == NULL) {
		return NULL;
	}

	int ret = libusb_init(&hid->ctx);
	if (ret < 0) {
		U_LOG_E("StarVR: could not init libusb, %s", libusb_error_name(ret));
		free(hid);
		return NULL;
	}

	hid->dev = libusb_open_device_with_vid_pid(hid->ctx, xpdev->vendor_id, xpdev->product_id);
	if (hid->dev == NULL) {
		U_LOG_E("StarVR: could not open %04x:%04x, is the udev rule installed?", xpdev->vendor_id,
		        xpdev->product_id);
		goto err;
	}

	/*
	 * usbhid binds this interface, so it has to be handed over before the
	 * endpoints can be talked to and handed back when we are done.
	 */
	if (libusb_kernel_driver_active(hid->dev, STARVR_INTERFACE) == 1) {
		ret = libusb_detach_kernel_driver(hid->dev, STARVR_INTERFACE);
		if (ret < 0) {
			U_LOG_E("StarVR: could not detach the kernel driver, %s", libusb_error_name(ret));
			goto err;
		}
		hid->detached = true;
	}

	ret = libusb_claim_interface(hid->dev, STARVR_INTERFACE);
	if (ret < 0) {
		U_LOG_E("StarVR: could not claim interface %d, %s", STARVR_INTERFACE, libusb_error_name(ret));
		goto err;
	}
	hid->claimed = true;

	return hid;

err:
	starvr_hid_close(hid);
	return NULL;
}

void
starvr_hid_close(struct starvr_hid *hid)
{
	if (hid == NULL) {
		return;
	}

	if (hid->dev != NULL) {
		if (hid->claimed) {
			libusb_release_interface(hid->dev, STARVR_INTERFACE);
		}
		if (hid->detached) {
			libusb_attach_kernel_driver(hid->dev, STARVR_INTERFACE);
		}
		libusb_close(hid->dev);
	}

	if (hid->ctx != NULL) {
		libusb_exit(hid->ctx);
	}

	free(hid);
}

bool
starvr_hid_eeprom_read(struct starvr_hid *hid, uint16_t address, uint16_t length, void *out_buffer)
{
	if (hid == NULL || out_buffer == NULL) {
		return false;
	}

	if ((uint32_t)address + (uint32_t)length > STARVR_EEPROM_SIZE) {
		return false;
	}

	uint8_t *dst = (uint8_t *)out_buffer;

	while (length > 0) {
		const uint8_t chunk = length >= STARVR_EEPROM_READ_CHUNK ? STARVR_EEPROM_READ_CHUNK : (uint8_t)length;

		bool got_it = false;

		for (int attempt = 0; attempt < STARVR_RETRY_COUNT && !got_it; attempt++) {
			struct starvr_eeprom_pkt pkt = {0};
			pkt.packet = STARVR_PACKET_EEPROM_REQUEST;
			pkt.sub = STARVR_EEPROM_SUB_READ;
			pkt.length = chunk;
			pkt.address_hi = (uint8_t)(address >> 8);
			pkt.address_lo = (uint8_t)address;

			if (!transfer_out(hid, &pkt)) {
				return false;
			}

			const uint8_t sent_hi = pkt.address_hi;
			const uint8_t sent_lo = pkt.address_lo;

			memset(&pkt, 0, sizeof(pkt));

			if (!transfer_in(hid, &pkt)) {
				return false;
			}

			if (pkt.packet != STARVR_PACKET_EEPROM_REPLY ||
			    pkt.sub != STARVR_EEPROM_SUB_READ ||
			    pkt.length != chunk ||
			    pkt.address_hi != sent_hi ||
			    pkt.address_lo != sent_lo) {
				continue;
			}

			if (pkt.status != 0) {
				return false;
			}

			memcpy(dst, pkt.data, chunk);
			got_it = true;
		}

		if (!got_it) {
			U_LOG_E("StarVR: EEPROM read at 0x%04x gave up", address);
			return false;
		}

		dst += chunk;
		address = (uint16_t)(address + chunk);
		length = (uint16_t)(length - chunk);
	}

	return true;
}

bool
starvr_hid_fps_setting_read(struct starvr_hid *hid, uint8_t *out_setting)
{
	struct starvr_ctrl_pkt reply = {0};

	if (!send_command(hid, STARVR_CMD_FPS_SETTING_READ, 0, NULL, &reply)) {
		return false;
	}

	*out_setting = reply.data[0];

	return true;
}

bool
starvr_hid_brightness_read(struct starvr_hid *hid, uint8_t out_values[2])
{
	struct starvr_ctrl_pkt reply = {0};

	if (!send_command(hid, STARVR_CMD_BRIGHTNESS_READ, 0, NULL, &reply)) {
		return false;
	}

	out_values[0] = reply.data[0];
	out_values[1] = reply.data[1];

	return true;
}

bool
starvr_hid_brightness_write(struct starvr_hid *hid, uint8_t channel, uint8_t value)
{
	if (channel >= 2) {
		return false;
	}

	const uint8_t payload[2] = {channel, value};

	return send_command(hid, STARVR_CMD_BRIGHTNESS_WRITE, sizeof(payload), payload, NULL);
}

bool
starvr_hid_display_on_read(struct starvr_hid *hid, bool *out_on)
{
	struct starvr_ctrl_pkt reply = {0};

	if (!send_command(hid, STARVR_CMD_DISPLAY_ON_OFF_STATUS_READ, 0, NULL, &reply)) {
		return false;
	}

	*out_on = reply.data[0] == 1;

	return true;
}

bool
starvr_hid_fw_version_read(struct starvr_hid *hid, char out_version[33])
{
	struct starvr_ctrl_pkt reply = {0};

	if (!send_command(hid, STARVR_CMD_FW_VERSION_READ, 0, NULL, &reply)) {
		return false;
	}

	memcpy(out_version, reply.data, 32);
	out_version[32] = '\0';

	return true;
}
