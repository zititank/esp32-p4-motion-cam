/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Part of the OV02C10 driver; see ov02c10.c for where the register values come from.
 */
#pragma once
#include <stdint.h>

#define OV02C10_REG_DELAY       0xFFFF
#define OV02C10_REG_MODE_SELECT 0x0100
#define OV02C10_MODE_STANDBY    0x00
#define OV02C10_MODE_STREAMING  0x01

typedef struct {
    uint16_t reg;
    uint8_t  val;
} ov02c10_reginfo_t;