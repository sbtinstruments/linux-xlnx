/* SPDX-License-Identifier: GPL-2.0-only */

/* Support to the Smart Pump Module from The Lee Company.
 * 
 * Inspired by: 
 * 
 * https://github.com/The-Lee-Company/arduino-control/blob/main/SPM%20I2C%20Arduino%20Demo/lee_ventus_reg.h 
 *
 * Copyright (c) 2024 SBT Instruments
 * 
 * Marcos Gonzalez Diaz <mgd@sbtinstruments.com>
 */
#ifndef _PUMPS_SPM_H
#define _PUMPS_SPM_H

#define SPM_READ_BIT BIT(7)

struct spm_write_int16_cmd {
	u8 reg;
	s16 val_le16;
} __attribute__((packed));

struct spm_write_float_cmd {
	u8 reg;
	s32 val_le32;
} __attribute__((packed));

#define SPM_SOURCE_SETVAL (0)
#define SPM_SOURCE_ANA1 (1)
#define SPM_SOURCE_ANA2 (2)
#define SPM_SOURCE_ANA3 (3)
#define SPM_SOURCE_FLOW (4)
#define SPM_SOURCE_DIGITAL_PRESSURE (5)

#define SPM_MODE_MANUAL (0)
#define SPM_MODE_PID (1)
#define SPM_MODE_BANGBANG (2)

#define SPM_DEVICE_TYPE_GP (2)
#define SPM_DEVICE_TYPE_SPM (3)

//***********************************************************************************
//* Regs list below
//***********************************************************************************

// -----------------------------------------------------------------------------
// General settings
// -----------------------------------------------------------------------------
#define SPM_REG_PUMP_ENABLE (0)
#define SPM_REG_POWER_LIMIT_MILLIWATTS (1)

#define SPM_REG_STREAM_MODE_ENABLE (2)

// -----------------------------------------------------------------------------
// Measurements
// -----------------------------------------------------------------------------
#define SPM_REG_MEAS_DRIVE_VOLTS (3)
#define SPM_REG_MEAS_DRIVE_MILLIAMPS (4)
#define SPM_REG_MEAS_DRIVE_MILLIWATTS (5)
#define SPM_REG_MEAS_DRIVE_FREQ (6)
#define SPM_REG_MEAS_ANA_1 (7)
#define SPM_REG_MEAS_ANA_2 (8)
#define SPM_REG_MEAS_ANA_3 (9)
#define SPM_REG_MEAS_FLOW (32)
#define SPM_REG_MEAS_DIGITAL_PRESSURE (39)
#define SPM_REG_MEAS_DRIVE_PHASE (41)

// -----------------------------------------------------------------------------
// Measurement settings
// -----------------------------------------------------------------------------
#define SPM_REG_SET_VAL (23)
#define SPM_REG_ANA_1_OFFSET (24)
#define SPM_REG_ANA_1_GAIN (25)
#define SPM_REG_ANA_2_OFFSET (26)
#define SPM_REG_ANA_2_GAIN (27)
#define SPM_REG_ANA_3_OFFSET (28)
#define SPM_REG_ANA_3_GAIN (29)
#define SPM_REG_DIGITAL_PRESSURE_OFFSET (40)

// -----------------------------------------------------------------------------
// Control settings
// -----------------------------------------------------------------------------

// Control mode
#define SPM_REG_CONTROL_MODE (10)

// Manual control settings
#define SPM_REG_MANUAL_MODE_SETPOINT_SOURCE (11)

// PID control settings
#define SPM_REG_PID_MODE_SETPOINT_SOURCE (12)
#define SPM_REG_PID_MODE_MEAS_SOURCE (13)
#define SPM_REG_PID_PROPORTIONAL_COEFF (14)
#define SPM_REG_PID_INTEGRAL_COEFF (15)
#define SPM_REG_PID_INTEGRAL_LIMIT_COEFF (16)
#define SPM_REG_PID_DIFFERENTIAL_COEFF (17)
#define SPM_REG_RESET_PID_ON_TURNON (33)

// Bang bang control settings
#define SPM_REG_BANG_BANG_MEAS_SOURCE (18)
#define SPM_REG_BANG_BANG_LOWER_THRESH (19)
#define SPM_REG_BANG_BANG_UPPER_THRESH (20)
#define SPM_REG_BANG_BANG_LOWER_POWER_MILLIWATTS (21)
#define SPM_REG_BANG_BANG_UPPER_POWER_MILLIWATTS (22)

// -----------------------------------------------------------------------------
// Miscellaneous settings
// -----------------------------------------------------------------------------
#define SPM_REG_STORE_CURRENT_SETTINGS (30)

#define SPM_REG_ERROR_CODE (31)

#define SPM_REG_USE_FREQUENCY_TRACKING (34)
#define SPM_REG_MANUAL_DRIVE_FREQUENCY (35)

#define SPM_REG_FIRMWARE_VERSION (36)
#define SPM_REG_DEVICE_TYPE (37)
#define SPM_REG_FIRMWARE_MINOR_VERSION (38)

// -----------------------------------------------------------------------------
// Communication settings
// -----------------------------------------------------------------------------
#define SPM_REG_DRIVER_I2C_ADDRESS (42)
#define SPM_REG_COMMUNICATION_INTERFACE (43)

#endif /* _PUMPS_SPM_H */
