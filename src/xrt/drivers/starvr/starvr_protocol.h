// Copyright 2026, Nyabsi
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  StarVR HMD protocol defines
 *
 * @author Nyabsi <nyabsi@sovellus.cc>
 * @ingroup drv_starvr
 */

#pragma once

#include <stdint.h>

#define STARVR_INTERFACE 0
#define STARVR_EP_IN 0x81
#define STARVR_EP_OUT 0x02

#define STARVR_XFER_SIZE 64
#define STARVR_USB_TIMEOUT_MS 100
#define STARVR_RETRY_COUNT 6

#define STARVR_PACKET_COMMAND_REQUEST 0xD0
#define STARVR_PACKET_COMMAND_REPLY 0xD1

#define STARVR_PACKET_EEPROM_REQUEST 0xBC
#define STARVR_PACKET_EEPROM_REPLY 0xCD

#define STARVR_EEPROM_SUB_WRITE 0x00
#define STARVR_EEPROM_SUB_READ 0x01

#define STARVR_EEPROM_SIZE 0x8000
#define STARVR_EEPROM_READ_CHUNK 58
#define STARVR_EEPROM_WRITE_CHUNK 32

#define STARVR_EEPROM_ADDR_UNIQUE_ID 0x0000
#define STARVR_EEPROM_ADDR_PANEL_SERIAL_LEFT 0x0900
#define STARVR_EEPROM_ADDR_PANEL_SERIAL_RIGHT 0x0940
#define STARVR_EEPROM_ADDR_CONFIG_LENGTH 0x1000
#define STARVR_EEPROM_ADDR_CONFIG 0x1020

//! What the EEPROM carries where the tracker serial goes before it is paired.
#define STARVR_UNPAIRED_TRACKER "need-pairing"

enum starvr_command_index
{
	STARVR_CMD_BRIGHTNESS_WRITE = 0,
	STARVR_CMD_INDEX_1 = 1,
	STARVR_CMD_INDEX_2 = 2,
	STARVR_CMD_INDEX_3 = 3,
	STARVR_CMD_INDEX_4 = 4,
	STARVR_CMD_FW_VERSION_READ = 5,
	STARVR_CMD_PCBA_VERSION_READ = 6,
	STARVR_CMD_INDEX_7 = 7,
	STARVR_CMD_DISPLAY_AUTO_ON_OFF_READ = 8,
	STARVR_CMD_DISPLAY_AUTO_ON_OFF_WRITE = 9,
	STARVR_CMD_DISPLAY_ON_OFF_STATUS_READ = 10,
	STARVR_CMD_FPS_SETTING_WRITE = 11,
	STARVR_CMD_FPS_SETTING_READ = 12,
	STARVR_CMD_DISPLAY_ON_TIME_READ = 13,
	STARVR_CMD_BRIGHTNESS_READ = 14,
	STARVR_CMD_INDEX_15 = 15,
	STARVR_CMD_INDEX_16 = 16,
	STARVR_CMD_INDEX_17 = 17,
	STARVR_CMD_HMD_POWER_CYCLE = 18,
	STARVR_CMD_INDEX_19 = 19,
	STARVR_CMD_INDEX_20 = 20,
	STARVR_CMD_INDEX_21 = 21,
};

#pragma pack(push, 1)

struct starvr_ctrl_pkt
{
	uint8_t packet;
	uint8_t opcode;
	uint8_t status;
	uint8_t data[STARVR_XFER_SIZE - 3];
};

struct starvr_eeprom_pkt
{
	uint8_t packet;
	uint8_t sub;
	uint8_t status;
	uint8_t length;
	uint8_t address_hi;
	uint8_t address_lo;
	uint8_t data[STARVR_XFER_SIZE - 6];
};

#pragma pack(pop)
