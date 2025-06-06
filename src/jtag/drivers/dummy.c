// SPDX-License-Identifier: GPL-2.0-or-later

/***************************************************************************
 *   Copyright (C) 2008 by Øyvind Harboe                                   *
 *   oyvind.harboe@zylin.com                                               *
 ***************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <jtag/interface.h>
#include "bitbang.h"
#include "hello.h"

/* my private tap controller state, which tracks state for calling code */
static enum tap_state dummy_state = TAP_RESET;

static int dummy_clock;		/* edge detector */

static int clock_count;		/* count clocks in any stable state, only stable states */

static uint32_t dummy_data;

static enum bb_value dummy_read(void)
{
	int data = 1 & dummy_data;
	dummy_data = (dummy_data >> 1) | (1 << 31);
	return data ? BB_HIGH : BB_LOW;
}

static int dummy_write(int tck, int tms, int tdi)
{
	/* TAP standard: "state transitions occur on rising edge of clock" */
	if (tck != dummy_clock) {
		if (tck) {
			enum tap_state old_state = dummy_state;
			dummy_state = tap_state_transition(old_state, tms);

			if (old_state != dummy_state) {
				if (clock_count) {
					LOG_DEBUG("dummy_tap: %d stable clocks", clock_count);
					clock_count = 0;
				}

				LOG_DEBUG("dummy_tap: %s", tap_state_name(dummy_state));

#if defined(DEBUG)
				if (dummy_state == TAP_DRCAPTURE)
					dummy_data = 0x01255043;
#endif
			} else {
				/* this is a stable state clock edge, no change of state here,
				 * simply increment clock_count for subsequent logging
				 */
				++clock_count;
			}
		}
		dummy_clock = tck;
	}
	return ERROR_OK;
}

static int dummy_reset(int trst, int srst)
{
	dummy_clock = 0;

	if (trst || (srst && (jtag_get_reset_config() & RESET_SRST_PULLS_TRST)))
		dummy_state = TAP_RESET;

	LOG_DEBUG("reset to: %s", tap_state_name(dummy_state));
	return ERROR_OK;
}

static int dummy_led(bool on)
{
	return ERROR_OK;
}

static const struct bitbang_interface dummy_bitbang = {
		.read = &dummy_read,
		.write = &dummy_write,
		.blink = &dummy_led,
	};

static int dummy_khz(int khz, int *jtag_speed)
{
	if (khz == 0)
		*jtag_speed = 0;
	else
		*jtag_speed = 64000/khz;
	return ERROR_OK;
}

static int dummy_speed_div(int speed, int *khz)
{
	if (speed == 0)
		*khz = 0;
	else
		*khz = 64000/speed;

	return ERROR_OK;
}

static int dummy_speed(int speed)
{
	return ERROR_OK;
}

static int dummy_init(void)
{
	bitbang_interface = &dummy_bitbang;

	return ERROR_OK;
}

static int dummy_quit(void)
{
	return ERROR_OK;
}

static const struct command_registration dummy_command_handlers[] = {
	{
		.name = "dummy",
		.mode = COMMAND_ANY,
		.help = "dummy interface driver commands",
		.chain = hello_command_handlers,
		.usage = "",
	},
	COMMAND_REGISTRATION_DONE,
};

/* The dummy driver is used to easily check the code path
 * where the target is unresponsive.
 */
static struct jtag_interface dummy_interface = {
	.supported = DEBUG_CAP_TMS_SEQ,
	.execute_queue = &bitbang_execute_queue,
};

struct adapter_driver dummy_adapter_driver = {
	.name = "dummy",
	.transport_ids = TRANSPORT_JTAG,
	.transport_preferred_id = TRANSPORT_JTAG,
	.commands = dummy_command_handlers,

	.init = &dummy_init,
	.quit = &dummy_quit,
	.reset = &dummy_reset,
	.speed = &dummy_speed,
	.khz = &dummy_khz,
	.speed_div = &dummy_speed_div,

	.jtag_ops = &dummy_interface,
};
