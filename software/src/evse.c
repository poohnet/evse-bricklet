/* evse-bricklet
 * Copyright (C) 2020-2022 Olaf Lüke <olaf@tinkerforge.com>
 *
 * evse.c: EVSE implementation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "evse.h"

#include <float.h>

#include "configs/config_evse.h"
#include "bricklib2/hal/ccu4_pwm/ccu4_pwm.h"
#include "bricklib2/hal/system_timer/system_timer.h"
#include "bricklib2/logging/logging.h"
#include "bricklib2/utility/util_definitions.h"
#include "bricklib2/bootloader/bootloader.h"
#include "bricklib2/warp/contactor_check.h"

#include "ads1118.h"
#include "iec61851.h"
#include "lock.h"
#include "led.h"
#include "communication.h"
#include "charging_slot.h"

#define EVSE_RELAY_MONOFLOP_TIME 10000 // 10 seconds

EVSE evse;

void evse_set_output(const uint16_t cp_duty_cycle, const bool contactor) {
	evse_set_cp_duty_cycle(cp_duty_cycle);

	// If the contactor is to be enabled and the lock is currently
	// not completely closed, we start the locking procedure and return.
	// The contactor will only be enabled after the lock is closed.
#if 0
	if(contactor) {
		if(lock_get_state() != LOCK_STATE_CLOSE) {
			lock_set_locked(true);
			return;
		}
	}
#endif

	if(((bool)XMC_GPIO_GetInput(EVSE_RELAY_PIN)) != contactor) {
		if(((cp_duty_cycle == 0) || (cp_duty_cycle == 1000)) && (!contactor)) {
			// If the duty cycle is set to either 0% or 100% PWM and the contactor is supposed to be turned off,
			// it is possible that the WARP Charger wants to turn off the charging session while the car
			// still wants to charge. In this case we wait until the car actually stops charging and
			// changes the resistance back to 2700 ohm before we turn the contactor off.
			// This assures that the contactor is not switched under load.
			//
			// NOTE: In case of an emergency (for example a dc fault detection) the contactor is of course switched off
			//       immediately and directly in the fault detection code without any regard to charging-state,
			//       PWM value, resistance or similar.
			//       This function is only called in non-emergency cases.

			if(ads1118.cp_pe_resistance <= IEC61851_CP_RESISTANCE_STATE_B) {
				if(evse.contactor_turn_off_time == 0) {
					evse.contactor_turn_off_time = system_timer_get_ms();
					return;
				} else if(system_timer_is_time_elapsed_ms(evse.contactor_turn_off_time, 3*1000)) {
					// The car has to respond within 3 seconds (see IEC 61851-1 standard table A.6 sequence 10.1),
					// thus after 3 seconds we turn the contactor off, even if the car has not yet responded yet.
					// In this case there may be some kind of communication error between wallbox and car and it
					// is better to turn the contactor off, even if still under load.
					evse.contactor_turn_off_time = 0;
				} else {
					return;
				}
			} else {
				evse.contactor_turn_off_time = 0;
			}
		}


		// Ignore all ADC measurements for a while if the contactor is
		// switched on or off, to be sure that the resulting EMI spike does
		// not give us a wrong measurement.
		ads1118.cp_invalid_counter = MAX(4, ads1118.cp_invalid_counter);
		ads1118.pp_invalid_counter = MAX(4, ads1118.pp_invalid_counter);

		// Also ignore contactor check for a while when contactor changes state
		contactor_check.invalid_counter = MAX(5, contactor_check.invalid_counter);

		if(contactor) {
			XMC_GPIO_SetOutputHigh(EVSE_RELAY_PIN);
		} else {
			XMC_GPIO_SetOutputLow(EVSE_RELAY_PIN);
		}
	}

#if 0
	if(!contactor) {
		if(lock_get_state() != LOCK_STATE_OPEN) {
			lock_set_locked(false);
		}
	}
#endif
}

// Check for presence of lock motor switch by checking between LED output and switch
void evse_init_lock_switch(void) {
// Remove lock switch support for now, it is not used by any WARP Charger
#if 0
#if LOGGING_LEVEL == LOGGING_NONE
	// Test if there is a connection between the GP output and the motor lock switch input
	// If there is, it means that the EVSE is configured to run without a motor lock switch input
	XMC_GPIO_SetOutputHigh(EVSE_OUTPUT_GP_PIN);
	system_timer_sleep_ms(50);
	const bool test1 = !XMC_GPIO_GetInput(EVSE_MOTOR_INPUT_SWITCH_PIN);

	XMC_GPIO_SetOutputLow(EVSE_OUTPUT_GP_PIN);
	system_timer_sleep_ms(50);
	const bool test2 = XMC_GPIO_GetInput(EVSE_MOTOR_INPUT_SWITCH_PIN);

	evse.has_lock_switch = !(test1 && test2);
#else
	evse.has_lock_switch = false;
#endif
#endif
	evse.has_lock_switch = false;
}

// Check pin header for max current
void evse_init_jumper(void) {
	const XMC_GPIO_CONFIG_t pin_config_input_tristate = {
		.mode             = XMC_GPIO_MODE_INPUT_TRISTATE,
		.input_hysteresis = XMC_GPIO_INPUT_HYSTERESIS_STANDARD
	};

	const XMC_GPIO_CONFIG_t pin_config_input_pullup = {
		.mode             = XMC_GPIO_MODE_INPUT_PULL_UP,
		.input_hysteresis = XMC_GPIO_INPUT_HYSTERESIS_STANDARD
	};

	const XMC_GPIO_CONFIG_t pin_config_input_pulldown = {
		.mode             = XMC_GPIO_MODE_INPUT_PULL_DOWN,
		.input_hysteresis = XMC_GPIO_INPUT_HYSTERESIS_STANDARD
	};

	XMC_GPIO_Init(EVSE_CONFIG_JUMPER_PIN0, &pin_config_input_pullup);
	XMC_GPIO_Init(EVSE_CONFIG_JUMPER_PIN1, &pin_config_input_pullup);
	system_timer_sleep_ms(50);
	bool pin0_pu = XMC_GPIO_GetInput(EVSE_CONFIG_JUMPER_PIN0);
	bool pin1_pu = XMC_GPIO_GetInput(EVSE_CONFIG_JUMPER_PIN1);

	XMC_GPIO_Init(EVSE_CONFIG_JUMPER_PIN0, &pin_config_input_pulldown);
	XMC_GPIO_Init(EVSE_CONFIG_JUMPER_PIN1, &pin_config_input_pulldown);
	system_timer_sleep_ms(50);
	bool pin0_pd = XMC_GPIO_GetInput(EVSE_CONFIG_JUMPER_PIN0);
	bool pin1_pd = XMC_GPIO_GetInput(EVSE_CONFIG_JUMPER_PIN1);

	XMC_GPIO_Init(EVSE_CONFIG_JUMPER_PIN0, &pin_config_input_tristate);
	XMC_GPIO_Init(EVSE_CONFIG_JUMPER_PIN1, &pin_config_input_tristate);

	// Differentiate between high, low and open
	char pin0 = 'x';
	if(pin0_pu && !pin0_pd) {
		pin0 = 'o';
	} else if(pin0_pu && pin0_pd) {
		pin0 = 'h';
	} else if(!pin0_pu && !pin0_pd) {
		pin0 = 'l';
	}

	char pin1 = 'x';
	if(pin1_pu && !pin1_pd) {
		pin1 = 'o';
	} else if(pin1_pu && pin1_pd) {
		pin1 = 'h';
	} else if(!pin1_pu && !pin1_pd) {
		pin1 = 'l';
	}

	if(pin0 == 'h' && pin1 == 'h') {
		evse.config_jumper_current = EVSE_CONFIG_JUMPER_UNCONFIGURED;
	} else if(pin0 == 'o' && pin1 == 'h') {
		evse.config_jumper_current = EVSE_CONFIG_JUMPER_CURRENT_6A;
	} else if(pin0 == 'l' && pin1 == 'h') {
		evse.config_jumper_current = EVSE_CONFIG_JUMPER_CURRENT_10A;
	} else if(pin0 == 'h' && pin1 == 'o') {
		evse.config_jumper_current = EVSE_CONFIG_JUMPER_CURRENT_13A;
	} else if(pin0 == 'o' && pin1 == 'o') {
		evse.config_jumper_current = EVSE_CONFIG_JUMPER_CURRENT_16A;
	} else if(pin0 == 'l' && pin1 == 'o') {
		evse.config_jumper_current = EVSE_CONFIG_JUMPER_CURRENT_20A;
	} else if(pin0 == 'h' && pin1 == 'l') {
		evse.config_jumper_current = EVSE_CONFIG_JUMPER_CURRENT_25A;
	} else if(pin0 == 'o' && pin1 == 'l') {
		evse.config_jumper_current = EVSE_CONFIG_JUMPER_CURRENT_32A;
	} else if(pin0 == 'l' && pin1 == 'l') {
		evse.config_jumper_current = EVSE_CONFIG_JUMPER_SOFTWARE;
	} else {
		evse.config_jumper_current = EVSE_CONFIG_JUMPER_UNCONFIGURED;
	}
}

void evse_load_calibration(void) {
	uint32_t page[EEPROM_PAGE_SIZE/sizeof(uint32_t)];
	bootloader_read_eeprom_page(EVSE_CALIBRATION_PAGE, page);

	// The magic number is not where it is supposed to be.
	// This is either our first startup or something went wrong.
	// We initialize the calibration data with sane default values and start a calibration.
	if(page[EVSE_CALIBRATION_MAGIC_POS] != EVSE_CALIBRATION_MAGIC) {
		ads1118.cp_cal_mul           = 1;
		ads1118.cp_cal_div           = 1;
		ads1118.cp_cal_diff_voltage  = -90; // -90 seems to be around average between all EVSEs we have tested, so we use it as default
		ads1118.cp_cal_2700ohm       = 0;
		for(uint8_t i = 0; i < ADS1118_880OHM_CAL_NUM; i++) {
			ads1118.cp_cal_880ohm[i] = 0;
		}
	} else {
		ads1118.cp_cal_mul           = page[EVSE_CALIBRATION_MUL_POS]      - INT16_MAX;
		ads1118.cp_cal_div           = page[EVSE_CALIBRATION_DIV_POS]      - INT16_MAX;
		ads1118.cp_cal_diff_voltage  = page[EVSE_CALIBRATION_DIFF_POS]     - INT16_MAX;
		ads1118.cp_cal_2700ohm       = page[EVSE_CALIBRATION_2700_POS]     - INT16_MAX;
		for(uint8_t i = 0; i < ADS1118_880OHM_CAL_NUM; i++) {
			ads1118.cp_cal_880ohm[i] = page[EVSE_CALIBRATION_880_POS + i]  - INT16_MAX;
		}
	}

	logd("Load calibration:\n\r");
	logd(" * mul %d, div %d, diff %d\n\r", ads1118.cp_cal_mul, ads1118.cp_cal_div, ads1118.cp_cal_diff_voltage);
	logd(" * 2700 Ohm: %d\n\r", ads1118.cp_cal_2700ohm);
	for(uint8_t i = 0; i < ADS1118_880OHM_CAL_NUM; i++) {
		logd(" * 800 Ohm %d: %d\n\r", i, ads1118.cp_cal_880ohm[i]);
	}
}

void evse_save_calibration(void) {
	uint32_t page[EEPROM_PAGE_SIZE/sizeof(uint32_t)];

	page[EVSE_CALIBRATION_MAGIC_POS]       = EVSE_CALIBRATION_MAGIC;
	page[EVSE_CALIBRATION_MUL_POS]         = ads1118.cp_cal_mul          + INT16_MAX;
	page[EVSE_CALIBRATION_DIV_POS]         = ads1118.cp_cal_div          + INT16_MAX;
	page[EVSE_CALIBRATION_DIFF_POS]        = ads1118.cp_cal_diff_voltage + INT16_MAX;
	page[EVSE_CALIBRATION_2700_POS]        = ads1118.cp_cal_2700ohm      + INT16_MAX;
	for(uint8_t i = 0; i < ADS1118_880OHM_CAL_NUM; i++) {
		page[EVSE_CALIBRATION_880_POS + i] = ads1118.cp_cal_880ohm[i]    + INT16_MAX;
	}

	bootloader_write_eeprom_page(EVSE_CALIBRATION_PAGE, page);
}

void evse_load_user_calibration(void) {
	uint32_t page[EEPROM_PAGE_SIZE/sizeof(uint32_t)];
	bootloader_read_eeprom_page(EVSE_USER_CALIBRATION_PAGE, page);

	// The magic number is not where it is supposed to be.
	// This is either our first startup or something went wrong.
	// We initialize the calibration data with sane default values and start a calibration.
	if(page[EVSE_USER_CALIBRATION_MAGIC_POS] != EVSE_USER_CALIBRATION_MAGIC) {
		ads1118.cp_user_cal_active        = false;
		ads1118.cp_user_cal_mul           = 1;
		ads1118.cp_user_cal_div           = 1;
		ads1118.cp_user_cal_diff_voltage  = -90; // -90 seems to be around average between all EVSEs we have tested, so we use it as default
		ads1118.cp_user_cal_2700ohm       = 0;
		for(uint8_t i = 0; i < ADS1118_880OHM_CAL_NUM; i++) {
			ads1118.cp_user_cal_880ohm[i] = 0;
		}
	} else {
		ads1118.cp_user_cal_active        = page[EVSE_USER_CALIBRATION_ACTIV_POS];
		ads1118.cp_user_cal_mul           = page[EVSE_USER_CALIBRATION_MUL_POS]     - INT16_MAX;
		ads1118.cp_user_cal_div           = page[EVSE_USER_CALIBRATION_DIV_POS]     - INT16_MAX;
		ads1118.cp_user_cal_diff_voltage  = page[EVSE_USER_CALIBRATION_DIFF_POS]    - INT16_MAX;
		ads1118.cp_user_cal_2700ohm       = page[EVSE_USER_CALIBRATION_2700_POS]    - INT16_MAX;
		for(uint8_t i = 0; i < ADS1118_880OHM_CAL_NUM; i++) {
			ads1118.cp_user_cal_880ohm[i] = page[EVSE_USER_CALIBRATION_880_POS + i] - INT16_MAX;
		}
	}

	logd("Load user calibration:\n\r");
	logd(" * mul %d, div %d, diff %d\n\r", ads1118.cp_user_cal_mul, ads1118.cp_user_cal_div, ads1118.cp_user_cal_diff_voltage);
	logd(" * 2700 Ohm: %d\n\r", ads1118.cp_user_cal_2700ohm);
	for(uint8_t i = 0; i < ADS1118_880OHM_CAL_NUM; i++) {
		logd(" * 800 Ohm %d: %d\n\r", i, ads1118.cp_user_cal_880ohm[i]);
	}
}

void evse_save_user_calibration(void) {
	uint32_t page[EEPROM_PAGE_SIZE/sizeof(uint32_t)];

	page[EVSE_USER_CALIBRATION_MAGIC_POS]       = EVSE_USER_CALIBRATION_MAGIC;
	page[EVSE_USER_CALIBRATION_ACTIV_POS]       = ads1118.cp_user_cal_active;
	page[EVSE_USER_CALIBRATION_MUL_POS]         = ads1118.cp_user_cal_mul          + INT16_MAX;
	page[EVSE_USER_CALIBRATION_DIV_POS]         = ads1118.cp_user_cal_div          + INT16_MAX;
	page[EVSE_USER_CALIBRATION_DIFF_POS]        = ads1118.cp_user_cal_diff_voltage + INT16_MAX;
	page[EVSE_USER_CALIBRATION_2700_POS]        = ads1118.cp_user_cal_2700ohm      + INT16_MAX;
	for(uint8_t i = 0; i < ADS1118_880OHM_CAL_NUM; i++) {
		page[EVSE_USER_CALIBRATION_880_POS + i] = ads1118.cp_user_cal_880ohm[i]    + INT16_MAX;
	}

	bootloader_write_eeprom_page(EVSE_USER_CALIBRATION_PAGE, page);
}

void evse_load_config(void) {
	uint32_t page[EEPROM_PAGE_SIZE/sizeof(uint32_t)];
	bootloader_read_eeprom_page(EVSE_CONFIG_PAGE, page);

	// The magic number is not where it is supposed to be.
	// This is either our first startup or something went wrong.
	// We initialize the config data with sane default values.
	if(page[EVSE_CONFIG_MAGIC_POS] != EVSE_CONFIG_MAGIC) {
		evse.legacy_managed = false;
	} else {
		evse.legacy_managed = page[EVSE_CONFIG_MANAGED_POS];
	}

	if(page[EVSE_CONFIG_MAGIC2_POS] != EVSE_CONFIG_MAGIC2) {
		evse.boost_mode_enabled = false;
	} else {
		evse.boost_mode_enabled = page[EVSE_CONFIG_BOOST_POS];
	}

	bool external_control_slot_to_default = false;
	// We use MAGIC6 to check if the new handling for external control is already active.
	// If the magic is not set, we activate the external control slot and set proper default values.
	// After that we set the new magic, so this only happens after first update.
	if(page[EVSE_CONFIG_MAGIC3_POS] != EVSE_CONFIG_MAGIC3) {
		external_control_slot_to_default = true;
	}

	// Handle charging slot defaults
	EVSEChargingSlotDefault *slot_default = (EVSEChargingSlotDefault *)(&page[EVSE_CONFIG_SLOT_DEFAULT_POS]);
	if(slot_default->magic == EVSE_CONFIG_SLOT_MAGIC) {
		for(uint8_t i = 0; i < 18; i++) {
			charging_slot.max_current_default[i]         = slot_default->current[i];
			charging_slot.active_default[i]              = slot_default->active_clear[i] & 1;
			charging_slot.clear_on_disconnect_default[i] = slot_default->active_clear[i] & 2;
		}
	} else {
		// If there is no default the button slot is activated and everything else is deactivated
		for(uint8_t i = 0; i < 18; i++) {
			charging_slot.max_current_default[i]         = 32000;
			charging_slot.active_default[i]              = false;
			charging_slot.clear_on_disconnect_default[i] = false;
		}

		// The default indices are offset by 2 to the slot indices
		charging_slot.max_current_default[CHARGING_SLOT_BUTTON-2]         = 32000;
		charging_slot.active_default[CHARGING_SLOT_BUTTON-2]              = true;
		charging_slot.clear_on_disconnect_default[CHARGING_SLOT_BUTTON-2] = false;

		charging_slot.max_current_default[CHARGING_SLOT_LOAD_MANAGEMENT-2]         = 0;
		charging_slot.active_default[CHARGING_SLOT_LOAD_MANAGEMENT-2]              = evse.legacy_managed;
		charging_slot.clear_on_disconnect_default[CHARGING_SLOT_LOAD_MANAGEMENT-2] = evse.legacy_managed;
	}

	if(external_control_slot_to_default) {
		charging_slot.max_current_default[CHARGING_SLOT_EXTERNAL-2]         = 32000;
		charging_slot.active_default[CHARGING_SLOT_EXTERNAL-2]              = false;
		charging_slot.clear_on_disconnect_default[CHARGING_SLOT_EXTERNAL-2] = false;
	}

	logd("Load config:\n\r");
	logd(" * legacy managed    %d\n\r", evse.legacy_managed);
	logd(" * relener           %d\n\r", sdm630.relative_energy.data);
	logd(" * shutdown input    %d\n\r", evse.shutdown_input_configuration);
	logd(" * slot current      %d %d %d %d %d %d %d %d", charging_slot.max_current_default[0], charging_slot.max_current_default[1], charging_slot.max_current_default[2], charging_slot.max_current_default[3], charging_slot.max_current_default[4], charging_slot.max_current_default[5], charging_slot.max_current_default[6], charging_slot.max_current_default[7]);
	logd(" * slot active/clear %d %d %d %d %d %d %d %d", charging_slot.clear_on_disconnect_default[0], charging_slot.clear_on_disconnect_default[1], charging_slot.clear_on_disconnect_default[2], charging_slot.clear_on_disconnect_default[3], charging_slot.clear_on_disconnect_default[4], charging_slot.clear_on_disconnect_default[5], charging_slot.clear_on_disconnect_default[6], charging_slot.clear_on_disconnect_default[7]);
}

void evse_save_config(void) {
	uint32_t page[EEPROM_PAGE_SIZE/sizeof(uint32_t)];

	page[EVSE_CONFIG_MAGIC_POS]          = EVSE_CONFIG_MAGIC;
	page[EVSE_CONFIG_MANAGED_POS]        = evse.legacy_managed;

	// Handle charging slot defaults
	EVSEChargingSlotDefault *slot_default = (EVSEChargingSlotDefault *)(&page[EVSE_CONFIG_SLOT_DEFAULT_POS]);
	for(uint8_t i = 0; i < 18; i++) {
		slot_default->current[i]      = charging_slot.max_current_default[i];
		slot_default->active_clear[i] = (charging_slot.active_default[i] << 0) | (charging_slot.clear_on_disconnect_default[i] << 1);
	}
	slot_default->magic = EVSE_CONFIG_SLOT_MAGIC;

	page[EVSE_CONFIG_MAGIC2_POS] = EVSE_CONFIG_MAGIC2;
	page[EVSE_CONFIG_BOOST_POS]  = evse.boost_mode_enabled;
	page[EVSE_CONFIG_MAGIC3_POS] = EVSE_CONFIG_MAGIC3;

	bootloader_write_eeprom_page(EVSE_CONFIG_PAGE, page);
}

void evse_factory_reset(void) {
	uint32_t page[EEPROM_PAGE_SIZE/sizeof(uint32_t)] = {0};
	bootloader_write_eeprom_page(EVSE_CONFIG_PAGE, page);

	NVIC_SystemReset();
}

uint16_t evse_get_cp_duty_cycle(void) {
	uint16_t duty_cycle = (64000 - ccu4_pwm_get_duty_cycle(EVSE_CP_PWM_SLICE_NUMBER))/64;

	return duty_cycle;
}

void evse_set_cp_duty_cycle(uint16_t duty_cycle) {
	const bool contactor_active = XMC_GPIO_GetInput(EVSE_RELAY_PIN);
	const bool use_16a = !contactor_active && (duty_cycle != 0) && (duty_cycle != 1000);
	if(use_16a) {
		duty_cycle = 266;
	}

	const uint16_t current_cp_duty_cycle = evse_get_cp_duty_cycle();
	const uint16_t new_cp_duty_cycle     = (uint16_t)(64000 - duty_cycle*64);

	if(current_cp_duty_cycle != duty_cycle) {
		// Ignore the next 10 ADC measurements between CP/PE after we
		// change PWM duty cycle of CP to be sure that that the measurement
		// is not of any in-between state.
		ads1118.cp_invalid_counter = MAX(2, ads1118.cp_invalid_counter);
		ccu4_pwm_set_duty_cycle(EVSE_CP_PWM_SLICE_NUMBER, new_cp_duty_cycle);
	}
}

void evse_init(void) {
	const XMC_GPIO_CONFIG_t pin_config_output = {
		.mode             = XMC_GPIO_MODE_OUTPUT_PUSH_PULL,
		.output_level     = XMC_GPIO_OUTPUT_LEVEL_LOW
	};

	const XMC_GPIO_CONFIG_t pin_config_input = {
		.mode             = XMC_GPIO_MODE_INPUT_TRISTATE,
		.input_hysteresis = XMC_GPIO_INPUT_HYSTERESIS_STANDARD
	};

	XMC_GPIO_Init(EVSE_RELAY_PIN,        &pin_config_output);
	XMC_GPIO_Init(EVSE_MOTOR_PHASE_PIN,  &pin_config_output);
#if LOGGING_LEVEL == LOGGING_NONE
	XMC_GPIO_Init(EVSE_OUTPUT_GP_PIN,    &pin_config_output);
#endif

	XMC_GPIO_Init(EVSE_MOTOR_INPUT_SWITCH_PIN, &pin_config_input);
	XMC_GPIO_Init(EVSE_INPUT_GP_PIN,           &pin_config_input);

	ccu4_pwm_init(EVSE_CP_PWM_PIN, EVSE_CP_PWM_SLICE_NUMBER, EVSE_CP_PWM_PERIOD-1); // 1kHz
	ccu4_pwm_set_duty_cycle(EVSE_CP_PWM_SLICE_NUMBER, 0);

	ccu4_pwm_init(EVSE_MOTOR_ENABLE_PIN, EVSE_MOTOR_ENABLE_SLICE_NUMBER, EVSE_MOTOR_PWM_PERIOD-1); // 10 kHz
	ccu4_pwm_set_duty_cycle(EVSE_MOTOR_ENABLE_SLICE_NUMBER, EVSE_MOTOR_PWM_PERIOD);

	evse.calibration_state = 0;
	evse.config_jumper_current_software = 6000; // default software configuration is 6A
	evse.max_current_configured = 32000; // default user defined current ist 32A
	evse.boost_mode_enabled = false;
	evse.boost_current = 0;

	evse_load_calibration();
	evse_load_user_calibration();
	evse_load_config();
	evse_init_jumper();
	evse_init_lock_switch();

	evse.startup_time = system_timer_get_ms();
	evse.car_stopped_charging = false;
	evse.communication_watchdog_time = 0;
	evse.contactor_turn_off_time = 0;
}

void evse_tick_debug(void) {
#if LOGGING_LEVEL != LOGGING_NONE
	static uint32_t debug_time = 0;
	if(system_timer_is_time_elapsed_ms(debug_time, 250)) {
		debug_time = system_timer_get_ms();
		uartbb_printf("\n\r");
		uartbb_printf("IEC61851 State: %d\n\r", iec61851.state);
		uartbb_printf("Has lock switch: %d\n\r", evse.has_lock_switch);
		uartbb_printf("Jumper configuration: %d\n\r", evse.config_jumper_current);
		uartbb_printf("LED State: %d\n\r", led.state);
		uartbb_printf("Resistance: CP %d, PP %d\n\r", ads1118.cp_pe_resistance, ads1118.pp_pe_resistance);
		uartbb_printf("CP PWM duty cycle: %d\n\r", ccu4_pwm_get_duty_cycle(EVSE_CP_PWM_SLICE_NUMBER));
		uartbb_printf("Contactor Check: AC1 %d, AC2 %d, State: %d, Error: %d\n\r", contactor_check.ac1_edge_count, contactor_check.ac2_edge_count, contactor_check.state, contactor_check.error);
		uartbb_printf("GPIO: Input %d, Output %d\n\r", XMC_GPIO_GetInput(EVSE_INPUT_GP_PIN), XMC_GPIO_GetInput(EVSE_OUTPUT_GP_PIN));
		uartbb_printf("Lock State: %d\n\r", lock.state);
	}
#endif
}

void evse_tick(void) {
	// Wait 12 seconds on first startup for DC-Wächter calibration
	if(evse.startup_time != 0 && !system_timer_is_time_elapsed_ms(evse.startup_time, 12000)) {
#if 0
		// According to Alcona it is OK to calibrate during startup if
		// a car is connected as long as the contactor doesn't activate.
		if(evse.calibration_error || ((ads1118.cp_voltage_calibrated != 0) && (ads1118.cp_voltage_calibrated < 11000))) {
			evse.calibration_error = true;
			led_set_blinking(3);
		}
#endif
		return;
	}


	if(evse.factory_reset_time != 0) {
		if(system_timer_is_time_elapsed_ms(evse.factory_reset_time, 500)) {
			evse_factory_reset();
		}
	}

	// Turn LED on (LED flicker off after startup/calibration)
	if(evse.startup_time != 0) {
		evse.startup_time = 0;
		led_set_on(false);
	}

	if(evse.calibration_state != 0) {
		// Nothing here
		// calibration is done externally through API.
		// We don't change anything while calibration is running
	} else if(evse.calibration_error) {
		led_set_blinking(3);
	} else {
		// Otherwise we implement the EVSE according to IEC 61851.
		iec61851_tick();
	}

	// Restart EVSE after 5 minutes without any communication with a Brick
	if((evse.communication_watchdog_time != 0) && system_timer_is_time_elapsed_ms(evse.communication_watchdog_time, 1000*60*5)) {
		// Only restart EVSE if brick-communication-watchdog triggers if no car is connected
		if(iec61851.state == IEC61851_STATE_A) {
			NVIC_SystemReset();
		}
	}

//	evse_tick_debug();
}
