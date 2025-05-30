// SPDX-License-Identifier: GPL-2.0-or-later

/**
 * @file
 * This file implements support for the ARM CoreSight components Trace Port
 * Interface Unit (TPIU) and Serial Wire Output (SWO). It also supports the
 * CoreSight TPIU-Lite and the special TPIU version present with Cortex-M3
 * and Cortex-M4 (that includes SWO).
 */

/*
 * Relevant specifications from ARM include:
 *
 * CoreSight(tm) Components Technical Reference Manual           ARM DDI 0314H
 * CoreSight(tm) TPIU-Lite Technical Reference Manual            ARM DDI 0317A
 * Cortex(tm)-M3 Technical Reference Manual                      ARM DDI 0337G
 * Cortex(tm)-M4 Technical Reference Manual                      ARM DDI 0439B
 * CoreSight(tm) SoC-400 Technical Reference Manual              ARM DDI 0480F
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdlib.h>
#include <jim.h>

#include <helper/bits.h>
#include <helper/command.h>
#include <helper/jim-nvp.h>
#include <helper/list.h>
#include <helper/log.h>
#include <helper/types.h>
#include <jtag/interface.h>
#include <server/server.h>
#include <target/arm_adi_v5.h>
#include <target/target.h>
#include <transport/transport.h>
#include "arm_tpiu_swo.h"

/* START_DEPRECATED_TPIU */
#include <target/cortex_m.h>
#define MSG "DEPRECATED \'tpiu config\' command: "
/* END_DEPRECATED_TPIU */

#define TCP_SERVICE_NAME                "tpiu_swo_trace"

/* default for Cortex-M3 and Cortex-M4 specific TPIU */
#define TPIU_SWO_DEFAULT_BASE           0xE0040000

#define TPIU_SSPSR_OFFSET               0x000
#define TPIU_CSPSR_OFFSET               0x004
#define TPIU_ACPR_OFFSET                0x010
#define TPIU_SPPR_OFFSET                0x0F0
#define TPIU_FFSR_OFFSET                0x300
#define TPIU_FFCR_OFFSET                0x304
#define TPIU_FSCR_OFFSET                0x308
#define TPIU_DEVID_OFFSET               0xfc8

#define TPIU_ACPR_MAX_PRESCALER         0x1fff
#define TPIU_SPPR_PROTOCOL_SYNC         (TPIU_PIN_PROTOCOL_SYNC)
#define TPIU_SPPR_PROTOCOL_MANCHESTER   (TPIU_PIN_PROTOCOL_ASYNC_MANCHESTER)
#define TPIU_SPPR_PROTOCOL_UART         (TPIU_PIN_PROTOCOL_ASYNC_UART)
#define TPIU_DEVID_NOSUPPORT_SYNC       BIT(9)
#define TPIU_DEVID_SUPPORT_MANCHESTER   BIT(10)
#define TPIU_DEVID_SUPPORT_UART         BIT(11)

enum arm_tpiu_swo_event {
	TPIU_SWO_EVENT_PRE_ENABLE,
	TPIU_SWO_EVENT_POST_ENABLE,
	TPIU_SWO_EVENT_PRE_DISABLE,
	TPIU_SWO_EVENT_POST_DISABLE,
};

static const struct jim_nvp nvp_arm_tpiu_swo_event[] = {
	{ .value = TPIU_SWO_EVENT_PRE_ENABLE,   .name = "pre-enable" },
	{ .value = TPIU_SWO_EVENT_POST_ENABLE,  .name = "post-enable" },
	{ .value = TPIU_SWO_EVENT_PRE_DISABLE,  .name = "pre-disable" },
	{ .value = TPIU_SWO_EVENT_POST_DISABLE, .name = "post-disable" },
};

struct arm_tpiu_swo_event_action {
	enum arm_tpiu_swo_event event;
	Jim_Interp *interp;
	Jim_Obj *body;
	struct arm_tpiu_swo_event_action *next;
};

struct arm_tpiu_swo_object {
	struct list_head lh;
	struct adiv5_mem_ap_spot spot;
	struct adiv5_ap *ap;
	char *name;
	struct arm_tpiu_swo_event_action *event_action;
	/* record enable before init */
	bool deferred_enable;
	bool enabled;
	bool en_capture;
	/** Handle to output trace data in INTERNAL capture mode */
	/** Synchronous output port width */
	uint32_t port_width;
	FILE *file;
	/** output mode */
	unsigned int pin_protocol;
	/** Enable formatter */
	bool en_formatter;
	/** frequency of TRACECLKIN (usually matches HCLK) */
	unsigned int traceclkin_freq;
	/** SWO pin frequency */
	unsigned int swo_pin_freq;
	/** where to dump the captured output trace data */
	char *out_filename;
	/** track TCP connections */
	struct list_head connections;
	/* START_DEPRECATED_TPIU */
	bool recheck_ap_cur_target;
	/* END_DEPRECATED_TPIU */
};

struct arm_tpiu_swo_connection {
	struct list_head lh;
	struct connection *connection;
};

struct arm_tpiu_swo_priv_connection {
	struct arm_tpiu_swo_object *obj;
};

static OOCD_LIST_HEAD(all_tpiu_swo);

#define ARM_TPIU_SWO_TRACE_BUF_SIZE	4096

static int arm_tpiu_swo_poll_trace(void *priv)
{
	struct arm_tpiu_swo_object *obj = priv;
	uint8_t buf[ARM_TPIU_SWO_TRACE_BUF_SIZE];
	size_t size = sizeof(buf);
	struct arm_tpiu_swo_connection *c;

	int retval = adapter_poll_trace(buf, &size);
	if (retval != ERROR_OK || !size)
		return retval;

	target_call_trace_callbacks(/*target*/NULL, size, buf);

	if (obj->file) {
		if (fwrite(buf, 1, size, obj->file) == size) {
			fflush(obj->file);
		} else {
			LOG_ERROR("Error writing to the SWO trace destination file");
			return ERROR_FAIL;
		}
	}

	if (obj->out_filename[0] == ':')
		list_for_each_entry(c, &obj->connections, lh)
			if (connection_write(c->connection, buf, size) != (int)size)
				LOG_ERROR("Error writing to connection"); /* FIXME: which connection? */

	return ERROR_OK;
}

static int arm_tpiu_swo_handle_event(struct arm_tpiu_swo_object *obj, enum arm_tpiu_swo_event event)
{
	for (struct arm_tpiu_swo_event_action *ea = obj->event_action; ea; ea = ea->next) {
		if (ea->event != event)
			continue;

		LOG_DEBUG("TPIU/SWO: %s event: %s (%d) action : %s",
			obj->name,
			jim_nvp_value2name_simple(nvp_arm_tpiu_swo_event, event)->name,
			event,
			Jim_GetString(ea->body, NULL));

		/* prevent event execution to change current target */
		struct command_context *cmd_ctx = current_command_context(ea->interp);
		struct target *saved_target = cmd_ctx->current_target;
		int retval = Jim_EvalObj(ea->interp, ea->body);
		cmd_ctx->current_target = saved_target;

		if (retval == JIM_RETURN)
			retval = ea->interp->returnCode;
		if (retval == JIM_OK || retval == ERROR_COMMAND_CLOSE_CONNECTION)
			return ERROR_OK;

		Jim_MakeErrorMessage(ea->interp);
		LOG_USER("Error executing event %s on TPIU/SWO %s:\n%s",
			jim_nvp_value2name_simple(nvp_arm_tpiu_swo_event, event)->name,
			obj->name,
			Jim_GetString(Jim_GetResult(ea->interp), NULL));
		/* clean both error code and stacktrace before return */
		Jim_Eval(ea->interp, "error \"\" \"\"");
		return ERROR_FAIL;
	}

	return ERROR_OK;
}

static void arm_tpiu_swo_close_output(struct arm_tpiu_swo_object *obj)
{
	if (obj->file) {
		fclose(obj->file);
		obj->file = NULL;
	}
	if (obj->out_filename[0] == ':')
		remove_service(TCP_SERVICE_NAME, &obj->out_filename[1]);
}

int arm_tpiu_swo_cleanup_all(void)
{
	struct arm_tpiu_swo_object *obj, *tmp;

	list_for_each_entry_safe(obj, tmp, &all_tpiu_swo, lh) {
		if (obj->enabled)
			arm_tpiu_swo_handle_event(obj, TPIU_SWO_EVENT_PRE_DISABLE);

		arm_tpiu_swo_close_output(obj);

		if (obj->en_capture) {
			target_unregister_timer_callback(arm_tpiu_swo_poll_trace, obj);

			int retval = adapter_config_trace(false, 0, 0, NULL, 0, NULL);
			if (retval != ERROR_OK)
				LOG_ERROR("Failed to stop adapter's trace");
		}

		if (obj->enabled)
			arm_tpiu_swo_handle_event(obj, TPIU_SWO_EVENT_POST_DISABLE);

		struct arm_tpiu_swo_event_action *ea = obj->event_action;
		while (ea) {
			struct arm_tpiu_swo_event_action *next = ea->next;
			Jim_DecrRefCount(ea->interp, ea->body);
			free(ea);
			ea = next;
		}

		if (obj->ap)
			dap_put_ap(obj->ap);

		free(obj->name);
		free(obj->out_filename);
		free(obj);
	}

	return ERROR_OK;
}

static int arm_tpiu_swo_service_new_connection(struct connection *connection)
{
	struct arm_tpiu_swo_priv_connection *priv = connection->service->priv;
	struct arm_tpiu_swo_object *obj = priv->obj;
	struct arm_tpiu_swo_connection *c = malloc(sizeof(*c));
	if (!c) {
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}
	c->connection = connection;
	list_add(&c->lh, &obj->connections);
	return ERROR_OK;
}

static int arm_tpiu_swo_service_input(struct connection *connection)
{
	/* read a dummy buffer to check if the connection is still active */
	long dummy;
	int bytes_read = connection_read(connection, &dummy, sizeof(dummy));

	if (bytes_read == 0) {
		return ERROR_SERVER_REMOTE_CLOSED;
	} else if (bytes_read == -1) {
		LOG_ERROR("error during read: %s", strerror(errno));
		return ERROR_SERVER_REMOTE_CLOSED;
	}

	return ERROR_OK;
}

static int arm_tpiu_swo_service_connection_closed(struct connection *connection)
{
	struct arm_tpiu_swo_priv_connection *priv = connection->service->priv;
	struct arm_tpiu_swo_object *obj = priv->obj;
	struct arm_tpiu_swo_connection *c, *tmp;

	list_for_each_entry_safe(c, tmp, &obj->connections, lh)
		if (c->connection == connection) {
			list_del(&c->lh);
			free(c);
			return ERROR_OK;
		}
	LOG_ERROR("Failed to find connection to close!");
	return ERROR_FAIL;
}

COMMAND_HANDLER(handle_arm_tpiu_swo_event_list)
{
	struct arm_tpiu_swo_object *obj = CMD_DATA;

	command_print(CMD, "Event actions for TPIU/SWO %s\n", obj->name);
	command_print(CMD, "%-25s | Body", "Event");
	command_print(CMD, "------------------------- | "
			"----------------------------------------");

	for (struct arm_tpiu_swo_event_action *ea = obj->event_action; ea; ea = ea->next) {
		struct jim_nvp *opt = jim_nvp_value2name_simple(nvp_arm_tpiu_swo_event, ea->event);
		command_print(CMD, "%-25s | %s",
				opt->name, Jim_GetString(ea->body, NULL));
	}
	command_print(CMD, "***END***");
	return ERROR_OK;
}

enum arm_tpiu_swo_cfg_param {
	CFG_PORT_WIDTH,
	CFG_PROTOCOL,
	CFG_FORMATTER,
	CFG_TRACECLKIN,
	CFG_BITRATE,
	CFG_OUTFILE,
	CFG_EVENT,
};

static const struct jim_nvp nvp_arm_tpiu_swo_config_opts[] = {
	{ .name = "-port-width",    .value = CFG_PORT_WIDTH },
	{ .name = "-protocol",      .value = CFG_PROTOCOL },
	{ .name = "-formatter",     .value = CFG_FORMATTER },
	{ .name = "-traceclk",      .value = CFG_TRACECLKIN },
	{ .name = "-pin-freq",      .value = CFG_BITRATE },
	{ .name = "-output",        .value = CFG_OUTFILE },
	{ .name = "-event",         .value = CFG_EVENT },
	/* handled by mem_ap_spot, added for jim_getopt_nvp_unknown() */
	{ .name = "-dap",           .value = -1 },
	{ .name = "-ap-num",        .value = -1 },
	{ .name = "-baseaddr",      .value = -1 },
	{ .name = NULL,             .value = -1 },
};

static const struct jim_nvp nvp_arm_tpiu_swo_protocol_opts[] = {
	{ .name = "sync",           .value = TPIU_SPPR_PROTOCOL_SYNC },
	{ .name = "uart",           .value = TPIU_SPPR_PROTOCOL_UART },
	{ .name = "manchester",     .value = TPIU_SPPR_PROTOCOL_MANCHESTER },
	{ .name = NULL,             .value = -1 },
};

static const struct jim_nvp nvp_arm_tpiu_swo_bool_opts[] = {
	{ .name = "on",             .value = 1 },
	{ .name = "yes",            .value = 1 },
	{ .name = "1",              .value = 1 },
	{ .name = "true",           .value = 1 },
	{ .name = "off",            .value = 0 },
	{ .name = "no",             .value = 0 },
	{ .name = "0",              .value = 0 },
	{ .name = "false",          .value = 0 },
	{ .name = NULL,             .value = -1 },
};

static int arm_tpiu_swo_configure(struct jim_getopt_info *goi, struct arm_tpiu_swo_object *obj)
{
	assert(obj);

	if (goi->is_configure && obj->enabled) {
		Jim_SetResultFormatted(goi->interp, "Cannot configure TPIU/SWO; %s is enabled!", obj->name);
		return JIM_ERR;
	}

	/* parse config or cget options ... */
	while (goi->argc > 0) {
		Jim_SetEmptyResult(goi->interp);

		int e = adiv5_jim_mem_ap_spot_configure(&obj->spot, goi);
		if (e == JIM_OK)
			continue;
		if (e == JIM_ERR)
			return e;

		struct jim_nvp *n;
		e = jim_getopt_nvp(goi, nvp_arm_tpiu_swo_config_opts, &n);
		if (e != JIM_OK) {
			jim_getopt_nvp_unknown(goi, nvp_arm_tpiu_swo_config_opts, 0);
			return e;
		}

		switch (n->value) {
		case CFG_PORT_WIDTH:
			if (goi->is_configure) {
				jim_wide port_width;
				e = jim_getopt_wide(goi, &port_width);
				if (e != JIM_OK)
					return e;
				if (port_width < 1 || port_width > 32) {
					Jim_SetResultString(goi->interp, "Invalid port width!", -1);
					return JIM_ERR;
				}
				obj->port_width = (uint32_t)port_width;
			} else {
				if (goi->argc)
					goto err_no_params;
				Jim_SetResult(goi->interp, Jim_NewIntObj(goi->interp, obj->port_width));
			}
			break;
		case CFG_PROTOCOL:
			if (goi->is_configure) {
				struct jim_nvp *p;
				e = jim_getopt_nvp(goi, nvp_arm_tpiu_swo_protocol_opts, &p);
				if (e != JIM_OK)
					return e;
				obj->pin_protocol = p->value;
			} else {
				if (goi->argc)
					goto err_no_params;
				struct jim_nvp *p;
				e = jim_nvp_value2name(goi->interp, nvp_arm_tpiu_swo_protocol_opts, obj->pin_protocol, &p);
				if (e != JIM_OK) {
					Jim_SetResultString(goi->interp, "protocol error", -1);
					return JIM_ERR;
				}
				Jim_SetResult(goi->interp, Jim_NewStringObj(goi->interp, p->name, -1));
			}
			break;
		case CFG_FORMATTER:
			if (goi->is_configure) {
				struct jim_nvp *p;
				e = jim_getopt_nvp(goi, nvp_arm_tpiu_swo_bool_opts, &p);
				if (e != JIM_OK)
					return e;
				obj->en_formatter = p->value;
			} else {
				if (goi->argc)
					goto err_no_params;
				struct jim_nvp *p;
				e = jim_nvp_value2name(goi->interp, nvp_arm_tpiu_swo_bool_opts, obj->en_formatter, &p);
				if (e != JIM_OK) {
					Jim_SetResultString(goi->interp, "formatter error", -1);
					return JIM_ERR;
				}
				Jim_SetResult(goi->interp, Jim_NewStringObj(goi->interp, p->name, -1));
			}
			break;
		case CFG_TRACECLKIN:
			if (goi->is_configure) {
				jim_wide clk;
				e = jim_getopt_wide(goi, &clk);
				if (e != JIM_OK)
					return e;
				obj->traceclkin_freq = clk;
			} else {
				if (goi->argc)
					goto err_no_params;
				Jim_SetResult(goi->interp, Jim_NewIntObj(goi->interp, obj->traceclkin_freq));
			}
			break;
		case CFG_BITRATE:
			if (goi->is_configure) {
				jim_wide clk;
				e = jim_getopt_wide(goi, &clk);
				if (e != JIM_OK)
					return e;
				obj->swo_pin_freq = clk;
			} else {
				if (goi->argc)
					goto err_no_params;
				Jim_SetResult(goi->interp, Jim_NewIntObj(goi->interp, obj->swo_pin_freq));
			}
			break;
		case CFG_OUTFILE:
			if (goi->is_configure) {
				const char *s;
				e = jim_getopt_string(goi, &s, NULL);
				if (e != JIM_OK)
					return e;
				if (s[0] == ':') {
					char *end;
					long port = strtol(s + 1, &end, 0);
					if (port <= 0 || port > UINT16_MAX || *end != '\0') {
						Jim_SetResultFormatted(goi->interp, "Invalid TCP port \'%s\'", s + 1);
						return JIM_ERR;
					}
				}
				char *out_filename = strdup(s);
				if (!out_filename) {
					LOG_ERROR("Out of memory");
					return JIM_ERR;
				}
				free(obj->out_filename);
				obj->out_filename = out_filename;
			} else {
				if (goi->argc)
					goto err_no_params;
				if (obj->out_filename)
					Jim_SetResult(goi->interp, Jim_NewStringObj(goi->interp, obj->out_filename, -1));
			}
			break;
		case CFG_EVENT:
			if (goi->is_configure) {
				if (goi->argc < 2) {
					Jim_WrongNumArgs(goi->interp, goi->argc, goi->argv, "-event ?event-name? ?EVENT-BODY?");
					return JIM_ERR;
				}
			} else {
				if (goi->argc != 1) {
					Jim_WrongNumArgs(goi->interp, goi->argc, goi->argv, "-event ?event-name?");
					return JIM_ERR;
				}
			}

			{
				struct jim_nvp *p;
				Jim_Obj *o;
				struct arm_tpiu_swo_event_action *ea = obj->event_action;

				e = jim_getopt_nvp(goi, nvp_arm_tpiu_swo_event, &p);
				if (e != JIM_OK) {
					jim_getopt_nvp_unknown(goi, nvp_arm_tpiu_swo_event, 1);
					return e;
				}

				while (ea) {
					/* replace existing? */
					if (ea->event == (enum arm_tpiu_swo_event)p->value)
						break;
					ea = ea->next;
				}

				if (goi->is_configure) {
					if (!ea) {
						ea = calloc(1, sizeof(*ea));
						if (!ea) {
							LOG_ERROR("Out of memory");
							return JIM_ERR;
						}
						ea->next = obj->event_action;
						obj->event_action = ea;
					}
					if (ea->body)
						Jim_DecrRefCount(ea->interp, ea->body);
					ea->event = p->value;
					ea->interp = goi->interp;
					jim_getopt_obj(goi, &o);
					ea->body = Jim_DuplicateObj(goi->interp, o);
					Jim_IncrRefCount(ea->body);
				} else {
					if (ea)
						Jim_SetResult(goi->interp, Jim_DuplicateObj(goi->interp, ea->body));
				}
			}
			break;
		}
	}

	return JIM_OK;

err_no_params:
	Jim_WrongNumArgs(goi->interp, goi->argc, goi->argv, "NO PARAMS");
	return JIM_ERR;
}

COMMAND_HANDLER(handle_arm_tpiu_swo_configure)
{
	struct arm_tpiu_swo_object *obj = CMD_DATA;

	if (!CMD_ARGC)
		return ERROR_COMMAND_SYNTAX_ERROR;

	struct jim_getopt_info goi;
	jim_getopt_setup(&goi, CMD_CTX->interp, CMD_ARGC, CMD_JIMTCL_ARGV);
	goi.is_configure = !strcmp(CMD_NAME, "configure");

	int e = arm_tpiu_swo_configure(&goi, obj);

	int reslen;
	const char *result = Jim_GetString(Jim_GetResult(CMD_CTX->interp), &reslen);
	if (reslen > 0)
		command_print(CMD, "%s", result);

	if (e != JIM_OK)
		return ERROR_FAIL;

	return ERROR_OK;
}

static int wrap_write_u32(struct target *target, struct adiv5_ap *tpiu_ap,
		target_addr_t address, uint32_t value)
{
	if (transport_is_hla())
		return target_write_u32(target, address, value);
	else
		return mem_ap_write_atomic_u32(tpiu_ap, address, value);
}

static int wrap_read_u32(struct target *target, struct adiv5_ap *tpiu_ap,
		target_addr_t address, uint32_t *value)
{
	if (transport_is_hla())
		return target_read_u32(target, address, value);
	else
		return mem_ap_read_atomic_u32(tpiu_ap, address, value);
}

static const struct service_driver arm_tpiu_swo_service_driver = {
	.name = "tpiu_swo_trace",
	.new_connection_during_keep_alive_handler = NULL,
	.new_connection_handler = arm_tpiu_swo_service_new_connection,
	.input_handler = arm_tpiu_swo_service_input,
	.connection_closed_handler = arm_tpiu_swo_service_connection_closed,
	.keep_client_alive_handler = NULL,
};

COMMAND_HANDLER(handle_arm_tpiu_swo_enable)
{
	struct arm_tpiu_swo_object *obj = CMD_DATA;
	uint32_t value;
	int retval;

	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (CMD_CTX->mode == COMMAND_CONFIG) {
		LOG_DEBUG("%s: enable deferred", obj->name);
		obj->deferred_enable = true;
		return ERROR_OK;
	}

	if (obj->enabled)
		return ERROR_OK;

	if (transport_is_hla() && obj->spot.ap_num != 0) {
		command_print(CMD,
			"Invalid access port 0x%" PRIx64 ". Only AP#0 allowed with hla transport",
			obj->spot.ap_num);
		return ERROR_FAIL;
	}

	if (!obj->traceclkin_freq) {
		command_print(CMD, "Trace clock-in frequency not set");
		return ERROR_FAIL;
	}

	const bool output_external = !strcmp(obj->out_filename, "external");

	if (obj->pin_protocol == TPIU_SPPR_PROTOCOL_MANCHESTER || obj->pin_protocol == TPIU_SPPR_PROTOCOL_UART) {
		if (!obj->swo_pin_freq) {
			if (output_external) {
				command_print(CMD, "SWO pin frequency required when using external capturing");
				return ERROR_FAIL;
			}

			LOG_DEBUG("SWO pin frequency not set, will be autodetected by the adapter");
		}
	}

	struct target *target = get_current_target(CMD_CTX);

	/* START_DEPRECATED_TPIU */
	if (obj->recheck_ap_cur_target) {
		if (strcmp(target_type_name(target), "cortex_m") &&
			strcmp(target_type_name(target), "hla_target")) {
			LOG_ERROR(MSG "Current target is not a Cortex-M nor a HLA");
			return ERROR_FAIL;
		}
		if (!target_was_examined(target)) {
			LOG_ERROR(MSG "Current target not examined yet");
			return ERROR_FAIL;
		}
		struct cortex_m_common *cm = target_to_cm(target);
		obj->recheck_ap_cur_target = false;
		obj->spot.ap_num = cm->armv7m.debug_ap->ap_num;
		if (obj->spot.ap_num == 0)
			LOG_INFO(MSG "Confirmed TPIU %s is on AP 0", obj->name);
		else
			LOG_INFO(MSG "Target %s is on AP#0x%" PRIx64 ". Revised command is "
				"\'tpiu create %s -dap %s -ap-num 0x%" PRIx64 "\'",
				target_name(target), obj->spot.ap_num,
				obj->name, adiv5_dap_name(obj->spot.dap), obj->spot.ap_num);
	}
	/* END_DEPRECATED_TPIU */

	if (!obj->ap) {
		obj->ap = dap_get_ap(obj->spot.dap, obj->spot.ap_num);
		if (!obj->ap) {
			command_print(CMD, "Cannot get AP");
			return ERROR_FAIL;
		}
	}

	/* trigger the event before any attempt to R/W in the TPIU/SWO */
	retval = arm_tpiu_swo_handle_event(obj, TPIU_SWO_EVENT_PRE_ENABLE);
	if (retval != ERROR_OK)
		return retval;

	retval = wrap_read_u32(target, obj->ap, obj->spot.base + TPIU_DEVID_OFFSET, &value);
	if (retval != ERROR_OK) {
		command_print(CMD, "Unable to read %s", obj->name);
		return retval;
	}
	switch (obj->pin_protocol) {
	case TPIU_SPPR_PROTOCOL_SYNC:
		value = !(value & TPIU_DEVID_NOSUPPORT_SYNC);
		break;
	case TPIU_SPPR_PROTOCOL_UART:
		value &= TPIU_DEVID_SUPPORT_UART;
		break;
	case TPIU_SPPR_PROTOCOL_MANCHESTER:
		value &= TPIU_DEVID_SUPPORT_MANCHESTER;
		break;
	default:
		value = 0;
	}
	if (!value) {
		struct jim_nvp *p = jim_nvp_value2name_simple(nvp_arm_tpiu_swo_protocol_opts, obj->pin_protocol);
		command_print(CMD, "%s does not support protocol %s", obj->name, p->name);
		return ERROR_FAIL;
	}

	if (obj->pin_protocol == TPIU_SPPR_PROTOCOL_SYNC) {
		retval = wrap_read_u32(target, obj->ap, obj->spot.base + TPIU_SSPSR_OFFSET, &value);
		if (retval != ERROR_OK) {
			command_print(CMD, "Cannot read TPIU register SSPSR");
			return retval;
		}
		if (!(value & BIT(obj->port_width - 1))) {
			command_print(CMD, "TPIU does not support port-width of %d bits", obj->port_width);
			return ERROR_FAIL;
		}
	}

	uint16_t prescaler = 1; /* dummy value */
	unsigned int swo_pin_freq = obj->swo_pin_freq; /* could be replaced */

	if (!output_external) {
		if (obj->out_filename[0] == ':') {
			struct arm_tpiu_swo_priv_connection *priv = malloc(sizeof(*priv));
			if (!priv) {
				LOG_ERROR("Out of memory");
				return ERROR_FAIL;
			}
			priv->obj = obj;
			LOG_INFO("starting trace server for %s on %s", obj->name, &obj->out_filename[1]);
			retval = add_service(&arm_tpiu_swo_service_driver, &obj->out_filename[1],
				CONNECTION_LIMIT_UNLIMITED, priv);
			if (retval != ERROR_OK) {
				command_print(CMD, "Can't configure trace TCP port %s", &obj->out_filename[1]);
				return retval;
			}
		} else if (strcmp(obj->out_filename, "-")) {
			obj->file = fopen(obj->out_filename, "ab");
			if (!obj->file) {
				command_print(CMD, "Can't open trace destination file \"%s\"", obj->out_filename);
				return ERROR_FAIL;
			}
		}

		retval = adapter_config_trace(true, obj->pin_protocol, obj->port_width,
			&swo_pin_freq, obj->traceclkin_freq, &prescaler);
		if (retval != ERROR_OK) {
			command_print(CMD, "Failed to start adapter's trace");
			arm_tpiu_swo_close_output(obj);
			return retval;
		}

		if (obj->pin_protocol == TPIU_SPPR_PROTOCOL_MANCHESTER || obj->pin_protocol == TPIU_SPPR_PROTOCOL_UART)
			if (!swo_pin_freq) {
				if (obj->swo_pin_freq)
					command_print(CMD, "Adapter rejected SWO pin frequency %d Hz", obj->swo_pin_freq);
				else
					command_print(CMD,
						"Adapter does not support auto-detection of SWO pin frequency nor a default value");

				arm_tpiu_swo_close_output(obj);
				return ERROR_FAIL;
			}

		if (obj->swo_pin_freq != swo_pin_freq)
			LOG_INFO("SWO pin data rate adjusted by adapter to %d Hz", swo_pin_freq);
		obj->swo_pin_freq = swo_pin_freq;

		target_register_timer_callback(arm_tpiu_swo_poll_trace, 1,
			TARGET_TIMER_TYPE_PERIODIC, obj);

		obj->en_capture = true;
	} else if (obj->pin_protocol == TPIU_SPPR_PROTOCOL_MANCHESTER || obj->pin_protocol == TPIU_SPPR_PROTOCOL_UART) {
		prescaler = (obj->traceclkin_freq + obj->swo_pin_freq / 2) / obj->swo_pin_freq;
		if (prescaler > TPIU_ACPR_MAX_PRESCALER)
			prescaler = TPIU_ACPR_MAX_PRESCALER;
		swo_pin_freq = obj->traceclkin_freq / prescaler;

		if (obj->swo_pin_freq != swo_pin_freq)
			LOG_INFO("SWO pin data rate adjusted to %d Hz", swo_pin_freq);
		obj->swo_pin_freq = swo_pin_freq;
	}

	retval = wrap_write_u32(target, obj->ap, obj->spot.base + TPIU_CSPSR_OFFSET, BIT(obj->port_width - 1));
	if (retval != ERROR_OK)
		goto error_exit;

	retval = wrap_write_u32(target, obj->ap, obj->spot.base + TPIU_ACPR_OFFSET, prescaler - 1);
	if (retval != ERROR_OK)
		goto error_exit;

	retval = wrap_write_u32(target, obj->ap, obj->spot.base + TPIU_SPPR_OFFSET, obj->pin_protocol);
	if (retval != ERROR_OK)
		goto error_exit;

	retval = wrap_read_u32(target, obj->ap, obj->spot.base + TPIU_FFCR_OFFSET, &value);
	if (retval != ERROR_OK)
		goto error_exit;
	if (obj->en_formatter)
		value |= BIT(1);
	else
		value &= ~BIT(1);
	retval = wrap_write_u32(target, obj->ap, obj->spot.base + TPIU_FFCR_OFFSET, value);
	if (retval != ERROR_OK)
		goto error_exit;

	retval = arm_tpiu_swo_handle_event(obj, TPIU_SWO_EVENT_POST_ENABLE);
	if (retval != ERROR_OK)
		goto error_exit;

	/* START_DEPRECATED_TPIU */
	target_handle_event(target, TARGET_EVENT_TRACE_CONFIG);
	/* END_DEPRECATED_TPIU */

	obj->enabled = true;
	return ERROR_OK;

error_exit:
	command_print(CMD, "Error!");

	if (obj->en_capture) {
		obj->en_capture = false;

		arm_tpiu_swo_close_output(obj);

		target_unregister_timer_callback(arm_tpiu_swo_poll_trace, obj);

		int retval1 = adapter_config_trace(false, 0, 0, NULL, 0, NULL);
		if (retval1 != ERROR_OK)
			command_print(CMD, "Failed to stop adapter's trace");
	}
	return retval;
}

COMMAND_HANDLER(handle_arm_tpiu_swo_disable)
{
	struct arm_tpiu_swo_object *obj = CMD_DATA;

	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (!obj->enabled)
		return ERROR_OK;
	obj->enabled = false;

	arm_tpiu_swo_handle_event(obj, TPIU_SWO_EVENT_PRE_DISABLE);

	if (obj->en_capture) {
		obj->en_capture = false;

		arm_tpiu_swo_close_output(obj);

		target_unregister_timer_callback(arm_tpiu_swo_poll_trace, obj);

		int retval = adapter_config_trace(false, 0, 0, NULL, 0, NULL);
		if (retval != ERROR_OK) {
			command_print(CMD, "Failed to stop adapter's trace");
			return retval;
		}
	}

	arm_tpiu_swo_handle_event(obj, TPIU_SWO_EVENT_POST_DISABLE);

	/* START_DEPRECATED_TPIU */
	struct target *target = get_current_target(CMD_CTX);
	target_handle_event(target, TARGET_EVENT_TRACE_CONFIG);
	/* END_DEPRECATED_TPIU */

	return ERROR_OK;
}

static const struct command_registration arm_tpiu_swo_instance_command_handlers[] = {
	{
		.name = "configure",
		.mode = COMMAND_ANY,
		.handler = handle_arm_tpiu_swo_configure,
		.help  = "configure a new TPIU/SWO for use",
		.usage = "[attribute value ...]",
	},
	{
		.name = "cget",
		.mode = COMMAND_ANY,
		.handler = handle_arm_tpiu_swo_configure,
		.help  = "returns the specified TPIU/SWO attribute",
		.usage = "attribute",
	},
	{
		.name = "eventlist",
		.mode = COMMAND_ANY,
		.handler = handle_arm_tpiu_swo_event_list,
		.help = "displays a table of events defined for this TPIU/SWO",
		.usage = "",
	},
	{
		.name = "enable",
		.mode = COMMAND_ANY,
		.handler = handle_arm_tpiu_swo_enable,
		.usage = "",
		.help = "Enables the TPIU/SWO output",
	},
	{
		.name = "disable",
		.mode = COMMAND_EXEC,
		.handler = handle_arm_tpiu_swo_disable,
		.usage = "",
		.help = "Disables the TPIU/SWO output",
	},
	COMMAND_REGISTRATION_DONE
};

COMMAND_HANDLER(handle_arm_tpiu_swo_create)
{
	int retval = ERROR_FAIL;

	if (!CMD_ARGC)
		return ERROR_COMMAND_SYNTAX_ERROR;

	/* does this command exist? */
	Jim_Cmd *jimcmd = Jim_GetCommand(CMD_CTX->interp, CMD_JIMTCL_ARGV[0], JIM_NONE);
	if (jimcmd) {
		command_print(CMD, "cannot create TPIU object because a command with name '%s' already exists",
			CMD_ARGV[0]);
		return ERROR_FAIL;
	}

	struct arm_tpiu_swo_object *obj = calloc(1, sizeof(struct arm_tpiu_swo_object));
	if (!obj) {
		LOG_ERROR("Out of memory");
		return ERROR_FAIL;
	}
	INIT_LIST_HEAD(&obj->connections);
	adiv5_mem_ap_spot_init(&obj->spot);
	obj->spot.base = TPIU_SWO_DEFAULT_BASE;
	obj->port_width = 1;
	obj->out_filename = strdup("external");
	if (!obj->out_filename) {
		LOG_ERROR("Out of memory");
		goto err_exit;
	}

	obj->name = strdup(CMD_ARGV[0]);
	if (!obj->name) {
		LOG_ERROR("Out of memory");
		goto err_exit;
	}

	/* Do the rest as "configure" options */
	struct jim_getopt_info goi;
	jim_getopt_setup(&goi, CMD_CTX->interp, CMD_ARGC - 1, CMD_JIMTCL_ARGV + 1);
	goi.is_configure = 1;
	int e = arm_tpiu_swo_configure(&goi, obj);

	int reslen;
	const char *result = Jim_GetString(Jim_GetResult(CMD_CTX->interp), &reslen);
	if (reslen > 0)
		command_print(CMD, "%s", result);

	if (e != JIM_OK)
		goto err_exit;

	if (!obj->spot.dap || obj->spot.ap_num == DP_APSEL_INVALID) {
		command_print(CMD, "-dap and -ap-num required when creating TPIU");
		goto err_exit;
	}

	/* now - create the new tpiu/swo name command */
	const struct command_registration obj_commands[] = {
		{
			.name = obj->name,
			.mode = COMMAND_ANY,
			.help = "tpiu/swo instance command group",
			.usage = "",
			.chain = arm_tpiu_swo_instance_command_handlers,
		},
		COMMAND_REGISTRATION_DONE
	};
	retval = register_commands_with_data(CMD_CTX, NULL, obj_commands, obj);
	if (retval != ERROR_OK)
		goto err_exit;

	list_add_tail(&obj->lh, &all_tpiu_swo);

	return ERROR_OK;

err_exit:
	free(obj->name);
	free(obj->out_filename);
	free(obj);
	return retval;
}

COMMAND_HANDLER(handle_arm_tpiu_swo_names)
{
	struct arm_tpiu_swo_object *obj;

	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	list_for_each_entry(obj, &all_tpiu_swo, lh)
		command_print(CMD, "%s", obj->name);

	return ERROR_OK;
}

COMMAND_HANDLER(handle_arm_tpiu_swo_init)
{
	struct arm_tpiu_swo_object *obj;
	int retval = ERROR_OK;

	if (CMD_ARGC != 0)
		return ERROR_COMMAND_SYNTAX_ERROR;

	list_for_each_entry(obj, &all_tpiu_swo, lh) {
		if (!obj->deferred_enable)
			continue;
		LOG_DEBUG("%s: running enable during init", obj->name);
		int retval2 = command_run_linef(CMD_CTX, "%s enable", obj->name);
		if (retval2 != ERROR_OK)
			retval = retval2;
	}
	return retval;
}

/* START_DEPRECATED_TPIU */
/* DEPRECATED: emulation of old command 'tpiu config' */
COMMAND_HANDLER(handle_tpiu_deprecated_config_command)
{
	struct target *target = get_current_target(CMD_CTX);
	struct arm_tpiu_swo_object *obj = NULL;
	int retval;

	if (strcmp(target_type_name(target), "cortex_m") &&
		strcmp(target_type_name(target), "hla_target")) {
		LOG_ERROR(MSG "Current target is not a Cortex-M nor a HLA");
		return ERROR_FAIL;
	}

	if (!list_empty(&all_tpiu_swo)) {
		obj = list_first_entry(&all_tpiu_swo, typeof(*obj), lh);
		LOG_INFO(MSG "Using %s", obj->name);
	} else {
		struct cortex_m_common *cm = target_to_cm(target);
		struct adiv5_private_config *pc = target->private_config;
		struct adiv5_dap *dap = pc->dap;
		uint64_t ap_num = pc->ap_num;
		bool set_recheck_ap_cur_target = false;

		LOG_INFO(MSG "Adding a TPIU \'%s.tpiu\' in the configuration", target_name(target));

		if (ap_num == DP_APSEL_INVALID && transport_is_hla())
			ap_num = 0; /* HLA should only support AP 0 */

		if (ap_num == DP_APSEL_INVALID && target_was_examined(target))
			ap_num = cm->armv7m.debug_ap->ap_num;

		if (ap_num == DP_APSEL_INVALID) {
			LOG_INFO(MSG "Target %s uses AP autodetection. Adding TPIU on AP 0; can be revised later",
				target_name(target));
			ap_num = 0;
			set_recheck_ap_cur_target = true;
		}

		LOG_INFO(MSG "Running: \'tpiu create %s.tpiu -dap %s -ap-num 0x%" PRIx64 "\'",
			target_name(target), adiv5_dap_name(dap), ap_num);

		retval = command_run_linef(CMD_CTX, "tpiu create %s.tpiu -dap %s -ap-num 0x%" PRIx64,
			target_name(target), adiv5_dap_name(dap), ap_num);
		if (retval != ERROR_OK)
			return retval;

		obj = list_first_entry(&all_tpiu_swo, typeof(*obj), lh);
		if (set_recheck_ap_cur_target)
			obj->recheck_ap_cur_target = true;
	}

	unsigned int cmd_idx = 0;
	if (cmd_idx == CMD_ARGC)
		return ERROR_COMMAND_SYNTAX_ERROR;

	if (!strcmp(CMD_ARGV[cmd_idx], "disable")) {
		if (CMD_ARGC != cmd_idx + 1)
			return ERROR_COMMAND_SYNTAX_ERROR;
		LOG_INFO(MSG "Running: \'%s disable\'", obj->name);
		return command_run_linef(CMD_CTX, "%s disable", obj->name);
	}

	const char *output = NULL;
	const char *protocol;
	const char *formatter = NULL;
	const char *port_width = NULL;
	const char *trace_clk;
	const char *pin_clk = NULL;
	if (!strcmp(CMD_ARGV[cmd_idx], "internal")) {
		cmd_idx++;
		if (cmd_idx == CMD_ARGC)
			return ERROR_COMMAND_SYNTAX_ERROR;
		output = CMD_ARGV[cmd_idx];
	} else if (strcmp(CMD_ARGV[cmd_idx], "external"))
		return ERROR_COMMAND_SYNTAX_ERROR;
	cmd_idx++;
	if (cmd_idx == CMD_ARGC)
		return ERROR_COMMAND_SYNTAX_ERROR;
	if (!strcmp(CMD_ARGV[cmd_idx], "sync")) {
		protocol = CMD_ARGV[cmd_idx];
		cmd_idx++;
		if (cmd_idx == CMD_ARGC)
			return ERROR_COMMAND_SYNTAX_ERROR;
		port_width = CMD_ARGV[cmd_idx];
	} else {
		if (strcmp(CMD_ARGV[cmd_idx], "manchester") && strcmp(CMD_ARGV[cmd_idx], "uart"))
			return ERROR_COMMAND_SYNTAX_ERROR;
		protocol = CMD_ARGV[cmd_idx];
		cmd_idx++;
		if (cmd_idx == CMD_ARGC)
			return ERROR_COMMAND_SYNTAX_ERROR;
		formatter = CMD_ARGV[cmd_idx];
	}
	cmd_idx++;
	if (cmd_idx == CMD_ARGC)
		return ERROR_COMMAND_SYNTAX_ERROR;
	trace_clk = CMD_ARGV[cmd_idx];
	cmd_idx++;
	if (cmd_idx != CMD_ARGC) {
		pin_clk = CMD_ARGV[cmd_idx];
		cmd_idx++;
	}
	if (cmd_idx != CMD_ARGC)
		return ERROR_COMMAND_SYNTAX_ERROR;

	LOG_INFO(MSG "Running: \'%s configure -protocol %s -traceclk %s" "%s%s" "%s%s" "%s%s" "%s%s\'",
		obj->name, protocol, trace_clk,
		pin_clk    ? " -pin-freq "   : "", pin_clk    ? pin_clk    : "",
		output     ? " -output "     : "", output     ? output     : "",
		formatter  ? " -formatter "  : "", formatter  ? formatter  : "",
		port_width ? " -port-width " : "", port_width ? port_width : "");

	retval = command_run_linef(CMD_CTX,
		"%s configure -protocol %s -traceclk %s" "%s%s" "%s%s" "%s%s" "%s%s",
		obj->name, protocol, trace_clk,
		pin_clk    ? " -pin-freq "   : "", pin_clk    ? pin_clk    : "",
		output     ? " -output "     : "", output     ? output     : "",
		formatter  ? " -formatter "  : "", formatter  ? formatter  : "",
		port_width ? " -port-width " : "", port_width ? port_width : "");
	if (retval != ERROR_OK)
		return retval;

	LOG_INFO(MSG "Running: \'%s enable\'", obj->name);
	retval = command_run_linef(CMD_CTX, "%s enable", obj->name);
	if (retval != ERROR_OK)
		return retval;

	return ERROR_OK;
}

static const struct command_registration arm_tpiu_deprecated_subcommand_handlers[] = {
	{
		.name = "config",
		.handler = handle_tpiu_deprecated_config_command,
		.mode = COMMAND_ANY,
		.help = "Configure TPIU features, DEPRECATED, use \'tpiu create\'",
		.usage = "(disable | "
		"((external | internal (<filename> | <:port> | -)) "
		"(sync <port width> | ((manchester | uart) <formatter enable>)) "
		"<TRACECLKIN freq> [<trace freq>]))",
	},
	COMMAND_REGISTRATION_DONE
};

const struct command_registration arm_tpiu_deprecated_command_handlers[] = {
	{
		.name = "tpiu",
		.chain = arm_tpiu_deprecated_subcommand_handlers,
		.usage = "",
		.help = "tpiu command group",
	},
	COMMAND_REGISTRATION_DONE
};
/* END_DEPRECATED_TPIU */

static const struct command_registration arm_tpiu_swo_subcommand_handlers[] = {
	{
		.name = "create",
		.mode = COMMAND_ANY,
		.handler = handle_arm_tpiu_swo_create,
		.usage = "name [-dap dap] [-ap-num num] [-baseaddr baseaddr]",
		.help = "Creates a new TPIU or SWO object",
	},
	{
		.name = "names",
		.mode = COMMAND_ANY,
		.handler = handle_arm_tpiu_swo_names,
		.usage = "",
		.help = "Lists all registered TPIU and SWO objects by name",
	},
	{
		.name = "init",
		.mode = COMMAND_EXEC,
		.handler = handle_arm_tpiu_swo_init,
		.usage = "",
		.help = "Initialize TPIU and SWO",
	},
	COMMAND_REGISTRATION_DONE
};

static const struct command_registration arm_tpiu_swo_command_handlers[] = {
	{
		.name = "tpiu",
		.chain = arm_tpiu_swo_subcommand_handlers,
		.usage = "",
		.help = "tpiu command group",
	},
	{
		.name = "swo",
		.chain = arm_tpiu_swo_subcommand_handlers,
		.usage = "",
		.help = "swo command group",
	},
	COMMAND_REGISTRATION_DONE
};

int arm_tpiu_swo_register_commands(struct command_context *cmd_ctx)
{
	return register_commands(cmd_ctx, NULL, arm_tpiu_swo_command_handlers);
}
