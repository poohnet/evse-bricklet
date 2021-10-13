/* evse-bricklet
 * Copyright (C) 2020 Olaf Lüke <olaf@tinkerforge.com>
 *
 * iec61851.c: Implementation of IEC 61851 EVSE state machine
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

#include "iec61851.h"

#include <stdint.h>
#include <string.h>

#include "bricklib2/utility/util_definitions.h"
#include "bricklib2/logging/logging.h"
#include "bricklib2/hal/ccu4_pwm/ccu4_pwm.h"
#include "configs/config_evse.h"
#include "ads1118.h"
#include "iec61851.h"
#include "lock.h"
#include "evse.h"
#include "contactor_check.h"
#include "led.h"
#include "button.h"

// Resistance between CP/PE
// inf  Ohm -> no car present
// 2700 Ohm -> car present
//  880 Ohm -> car charging
//  240 Ohm -> car charging with ventilation
// ==>
// > 10000 -> State A
// >  1790 -> State B
// >  560 -> State C
// >  150 -> State D
// <  150 -> State E/F
#define IEC61851_CP_RESISTANCE_STATE_A 10000
#define IEC61851_CP_RESISTANCE_STATE_B  1790
#define IEC61851_CP_RESISTANCE_STATE_C   300
#define IEC61851_CP_RESISTANCE_STATE_D   150

// Resistance between PP/PE
// 1000..2200 Ohm => 13A
// 330..1000 Ohm  => 20A
// 150..330 Ohm   => 32A
// 75..150 Ohm    => 63A
#define IEC61851_PP_RESISTANCE_13A 1000
#define IEC61851_PP_RESISTANCE_20A  330
#define IEC61851_PP_RESISTANCE_32A  150


IEC61851 iec61851;

void iec61851_set_state(IEC61851State state) {
	if(state != iec61851.state) {
		// If we change to state C and the charging timer was not started, we start it
		if((state == IEC61851_STATE_C) && (evse.charging_time == 0)) {
			evse.charging_time = system_timer_get_ms();
		}

		if((state == IEC61851_STATE_A) || (state == IEC61851_STATE_B)) {
			// Turn LED on with timer for standby if we have a state change to state A or B
			led_set_on(false);
		}

		if((iec61851.state != IEC61851_STATE_A) && (state == IEC61851_STATE_A)) {
			if(!evse.charging_autostart) {
				// If we change from state C to either A or B and autostart is disabled,
				// we set the buttom pressed flag.
				// This means that the user needs to call "StartCharging()" through the API
				// before the car starts to charge again.
				button.was_pressed = true;
			}

			// If state changed from C to A or B and the EVSE is managed externally, we invalidate the managed current
			if(evse.managed) {
				evse.max_managed_current = 0;
			}
		}

		iec61851.state             = state;
		iec61851.last_state_change = system_timer_get_ms();
	}
}

// TODO: We can find out that no cable is connected here
//       if resistance > 10000. Do we want to have a specific
//       state for that?
uint32_t iec61851_get_ma_from_pp_resistance(void) {
	if(ads1118.pp_pe_resistance >= 1000) {
		return 13000; // 13A
	} else if(ads1118.pp_pe_resistance >= 330) {
		return 20000; // 20A
	} else if(ads1118.pp_pe_resistance >= 150) {
		return 32000; // 32A
	} else {
		return 64000; // 64A
	}
}

uint32_t iec61851_get_ma_from_jumper(void) {
	switch(evse.config_jumper_current) {
		case EVSE_CONFIG_JUMPER_CURRENT_6A:  return 6000;
		case EVSE_CONFIG_JUMPER_CURRENT_10A: return 10000;
		case EVSE_CONFIG_JUMPER_CURRENT_13A: return 13000;
		case EVSE_CONFIG_JUMPER_CURRENT_16A: return 16000;
		case EVSE_CONFIG_JUMPER_CURRENT_20A: return 20000;
		case EVSE_CONFIG_JUMPER_CURRENT_25A: return 25000;
		case EVSE_CONFIG_JUMPER_CURRENT_32A: return 32000;
		case EVSE_CONFIG_JUMPER_SOFTWARE: return evse.config_jumper_current_software;
		default: return 6000;
	}
}

uint32_t iec61851_get_max_ma(void) {
	uint32_t max_conf_pp_jumper = MIN(evse.max_current_configured, MIN(iec61851_get_ma_from_pp_resistance(), iec61851_get_ma_from_jumper()));
	if(evse.managed) {
		return MIN(max_conf_pp_jumper, evse.max_managed_current);
	}

	return max_conf_pp_jumper;
}

// Duty cycle in pro mille (1/10 %)
uint16_t iec61851_get_duty_cycle_for_ma(uint32_t ma) {
	// Special case for managed mode.
	// In managed mode we support a temporary stop of charging without disconnecting the vehicle.
	if(ma == 0) {
		// 100% duty cycle => charging not allowed
		// we do 100% here instead of 0% (both mean charging not allowed) 
		// to be able to still properly measure the resistance that the car applies.
		return 1000; 
	}

	uint32_t duty_cycle;
	if(ma <= 51000) {
		duty_cycle = ma/60; // For 6A-51A: xA = %duty*0.6
	} else {
		duty_cycle = ma/250 + 640; // For 51A-80A: xA= (%duty - 64)*2.5
	}

	// The standard defines 8% as minimum and 100% as maximum
	return BETWEEN(80, duty_cycle, 1000); 
}

void iec61851_state_a(void) {
	// Apply +12V to CP, disable contactor
	evse_set_output(1000, false);

	if(ads1118.cp_pe_resistance > IEC61851_CP_RESISTANCE_STATE_A) {
		// If the button was released while in a different state,
		// we see the state change back to A as an event that turns the LED back on (until standby)
		if(button_reset()) {
			led_set_on(false);
		}
	}
}

void iec61851_state_b(void) {
	// Apply 1kHz square wave to CP with appropriate duty cycle, disable contactor
	uint32_t ma = iec61851_get_max_ma();
	evse_set_output(iec61851_get_duty_cycle_for_ma(ma), false);
}

void iec61851_state_c(void) {
	// Apply 1kHz square wave to CP with appropriate duty cycle, enable contactor
	uint32_t ma = iec61851_get_max_ma();
	evse_set_output(iec61851_get_duty_cycle_for_ma(ma), true);
	led_set_breathing();
}

void iec61851_state_d(void) {
	// State D is not supported
	// Apply +12V to CP, disable contactor
	evse_set_output(1000, false);
}

void iec61851_state_ef(void) {
	// In case of error apply +12V to CP, disable contactor
	evse_set_output(1000, false);
}

void iec61851_tick(void) {
	if(evse.calibration_state != 0) {
		return;
	}

	if(contactor_check.error != 0) {
		led_set_blinking(4);
		iec61851_set_state(IEC61851_STATE_EF);
	} else if(evse.config_jumper_current == EVSE_CONFIG_JUMPER_UNCONFIGURED) {
		// We don't allow the jumper to be unconfigured
		led_set_blinking(2);
		iec61851_set_state(IEC61851_STATE_EF);
	} else if(button.was_pressed) {
		iec61851_set_state(IEC61851_STATE_A);

		// As long as we are in "was_pressed"-state and the button is 
		// still pressed (or key is turned to off) the LED stays off
		if(button.state == BUTTON_STATE_PRESSED) {
			led_set_off();
		}
	} else {
		// Wait for ADC measurements to be valid
		if(ads1118.cp_invalid_counter > 0) {
			return;
		}

		// When an ID.3 is connected to the WARP charger and the duty cycle is already
		// below 100% (the wallbox is ready) but the contactor is not yet activated, the
		// ID.3 somtimes generates a spike in the resistance that we measure when it
		// engages the resistor to apply 880 ohm between CP/PE. We have not seen this in
		// other cars, we assume this is some kind of capacitive effect. To make sure
		// that we don't cancel the charging here, we increase the STATE A threshold for
		// this scenario.
		const uint16_t current_cp_duty_cycle = evse_get_cp_duty_cycle();
		const bool id3_mode = (current_cp_duty_cycle != 1000) && !XMC_GPIO_GetInput(EVSE_RELAY_PIN);
		if(!id3_mode) {
			iec61851.id3_mode_time = 0;
		}

		if(id3_mode && (ads1118.cp_pe_resistance > IEC61851_CP_RESISTANCE_STATE_A*3)) {
			if(iec61851.id3_mode_time == 0) {
				iec61851.id3_mode_time = system_timer_get_ms();
			} else {
				// wait for at least 500ms between B->A state change in ID.3 mode
				if(system_timer_is_time_elapsed_ms(iec61851.id3_mode_time, 500)) {
					iec61851_set_state(IEC61851_STATE_A);
				}
			}
		} else if(!id3_mode && (ads1118.cp_pe_resistance > IEC61851_CP_RESISTANCE_STATE_A)) {
			iec61851_set_state(IEC61851_STATE_A);
		} else if(ads1118.cp_pe_resistance > IEC61851_CP_RESISTANCE_STATE_B) {
			iec61851_set_state(IEC61851_STATE_B);
		} else if(ads1118.cp_pe_resistance > IEC61851_CP_RESISTANCE_STATE_C) {
			if(evse.managed && (evse.max_managed_current == 0)) {
				iec61851_set_state(IEC61851_STATE_B);
			} else {
				iec61851_set_state(IEC61851_STATE_C);
			}
		} else if(ads1118.cp_pe_resistance > IEC61851_CP_RESISTANCE_STATE_D) {
			led_set_blinking(5);
			iec61851_set_state(IEC61851_STATE_D);
		} else {
			led_set_blinking(5);
			iec61851_set_state(IEC61851_STATE_EF);
		}
	}

	switch(iec61851.state) {
		case IEC61851_STATE_A:  iec61851_state_a();  break;
		case IEC61851_STATE_B:  iec61851_state_b();  break;
		case IEC61851_STATE_C:  iec61851_state_c();  break;
		case IEC61851_STATE_D:  iec61851_state_d();  break;
		case IEC61851_STATE_EF: iec61851_state_ef(); break;
	}
}

void iec61851_init(void) {
	memset(&iec61851, 0, sizeof(IEC61851));
	iec61851.last_state_change = system_timer_get_ms();
}

