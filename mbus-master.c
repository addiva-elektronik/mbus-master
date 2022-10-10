/* Very basic M-Bus master with a limted CLI
 *
 * Copyright (C) 2022  Addiva Elektronik AB
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <err.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <stdio.h>
#include <mbus/mbus.h>

#define dbg(...) if (debug) warnx(__VA_ARGS__)
#define log(...)            warnx(__VA_ARGS__)

/* From The Practice of Programming, by Kernighan and Pike */
#ifndef NELEMS
#define NELEMS(array) (sizeof(array) / sizeof(array[0]))
#endif

static char *arg0 = "mbus-master";
static char *device;
static int   running = 1;
static int   interactive = 1;
static int   debug;


static int mbus_debug(mbus_handle *handle, int enable)
{
	if (enable) {
		mbus_register_send_event(handle, mbus_dump_send_event);
		mbus_register_recv_event(handle, mbus_dump_recv_event);
	} else {
		mbus_register_send_event(handle, NULL);
		mbus_register_recv_event(handle, NULL);
	}

	return 0;
}

/*
 * init slaves to get really the beginning of the records
 */
static int init_slaves(mbus_handle *handle)
{
	if (mbus_send_ping_frame(handle, MBUS_ADDRESS_NETWORK_LAYER, 1) == -1) {
	error:
		warnx("Failed initializing M-Bus slaves.");
		return -1;
	}

	if (mbus_send_ping_frame(handle, MBUS_ADDRESS_BROADCAST_NOREPLY, 1) == -1)
		goto error;

	return 0;
}

static int ping_address(mbus_handle *handle, mbus_frame *reply, int address)
{
	int i, rc = MBUS_RECV_RESULT_ERROR;

	memset(reply, 0, sizeof(mbus_frame));

	for (i = 0; i <= handle->max_search_retry; i++) {
		if (debug) {
			printf("%d ", address);
			fflush(stdout);
		}

		if (mbus_send_ping_frame(handle, address, 0) == -1) {
			warn("scan failed.  Failed sending ping frame: %s", mbus_error_str());
			return MBUS_RECV_RESULT_ERROR;
		}

		rc = mbus_recv_frame(handle, reply);
		if (rc != MBUS_RECV_RESULT_TIMEOUT)
			return rc;
	}

	return rc;
}

static int mbus_scan_1st_address_range(mbus_handle *handle)
{
	int address;
	int rc = 1;

	for (address = 0; address <= MBUS_MAX_PRIMARY_SLAVES; address++) {
		mbus_frame reply;
		int rc;

		if (!running)
			break;

		rc = ping_address(handle, &reply, address);
		if (rc == MBUS_RECV_RESULT_TIMEOUT)
			continue;

		if (rc == MBUS_RECV_RESULT_INVALID) {
			mbus_purge_frames(handle);
			warnx("collision at address %d.", address);
			continue;
		}

		if (mbus_frame_type(&reply) == MBUS_FRAME_TYPE_ACK) {
			if (mbus_purge_frames(handle)) {
				warnx("collision at address %d.", address);
				continue;
			}

			log("found an M-Bus device at address %d.", address);
			rc = 0;
		}
	}

	return rc;
}

static int scan_devices(mbus_handle *handle, char *args)
{
	(void)args;

	if (init_slaves(handle))
		return -1;

	return mbus_scan_1st_address_range(handle);
}

static int probe_devices(mbus_handle *handle, char *args)
{
	char *mask = "FFFFFFFFFFFFFFFF";

	if (init_slaves(handle))
		return 1;

	if (args)
		mask = args;

	if (!mbus_is_secondary_address(mask)) {
		warnx("malformed secondary address mask, must be 16 char HEX number.");
		return 1;
	}

	return mbus_scan_2nd_address_range(handle, 0, mask);
}

static int quit_program(mbus_handle *handle, char *args)
{
	(void)handle;
	(void)args;
	return running = 0;
}

static int secondary_select(mbus_handle *handle, char *mask)
{
	dbg("sending secondary select for mask %s", mask);

	switch (mbus_select_secondary_address(handle, mask)) {
	case MBUS_PROBE_COLLISION:
		warnx("address mask [%s] matches more than one device.", mask);
		return -1;
	case MBUS_PROBE_NOTHING:
		warnx("address mask [%s] does not match any device.", mask);
		return -1;
	case MBUS_PROBE_ERROR:
		warnx("failed selecting secondary address [%s].", mask);
		return -1;
	case MBUS_PROBE_SINGLE:
		dbg("address mask [%s] matches a single device.", mask);
		break;
	}        

	return MBUS_ADDRESS_NETWORK_LAYER;
}

int parse_addr(mbus_handle *handle, char *args)
{
	int address;

	if (!args) {
		warnx("missing required argument, address, can be primary or secondary.");
		return 1;
	}

	if (init_slaves(handle))
		return 1;

	if (mbus_is_secondary_address(args)) {
		if (secondary_select(handle, args) == -1)
			return 1;
		address = MBUS_ADDRESS_NETWORK_LAYER;
	} else {
		address = atoi(args);
		if (address < 1 || address > 255) {
			warnx("invalid primary address %s.", args);
			return 1;
		}
	}

	return address;
}

static int query_device(mbus_handle *handle, char *args)
{
	mbus_frame_data data;
	mbus_frame reply;
	char *xml_data;
	char *addr_arg;
	int address;

	addr_arg = strsep(&args, " \n\t");
	address = parse_addr(handle, addr_arg);
	if (mbus_send_request_frame(handle, address) == -1) {
		warnx("failed sending M-Bus request to %d.", address);
		return 1;
	}

	if (mbus_recv_frame(handle, &reply) != MBUS_RECV_RESULT_OK) {
		warnx("failed receiving M-Bus response from %d.", address);
		return 1;
	}

	if (mbus_frame_data_parse(&reply, &data) == -1) {
		warnx("M-bus data parse error: %s", mbus_error_str());
		return 1;
	}

	if (!args) {
		/* Dump entire response as XML */
		if (!(xml_data = mbus_frame_data_xml(&data))) {
			warnx("failed generating XML output of M-BUS response: %s", mbus_error_str());
			return 1;
		}

		printf("%s", xml_data);
		free(xml_data);

		if (data.data_var.record)
			mbus_data_record_free(data.data_var.record);
	} else {
		int record_id;

		/* Query for a single record */
		record_id = atoi(args);

		if (data.type == MBUS_DATA_TYPE_FIXED) {
			/* TODO: Implement this -- Not fixed in BCT --Jachim */
		}
		if (data.type == MBUS_DATA_TYPE_VARIABLE) {
			mbus_data_record *record;
			int i;

			for (record = data.data_var.record, i = 0; record; record = record->next, i++) {
				dbg("Record ID %d DIF %02x VID %02x", i,
				    record->drh.dib.dif & MBUS_DATA_RECORD_DIF_MASK_DATA,
				    record->drh.vib.vif & MBUS_DIB_VIF_WITHOUT_EXTENSION);
			}

			for (record = data.data_var.record, i = 0; record && i < record_id; record = record->next, i++)
				;

			if (i != record_id) {
				if (data.data_var.record)
					mbus_data_record_free(data.data_var.record);

				mbus_frame_free(reply.next);
				return 1;
			}

			uint32_t value;
			if (mbus_variable_value_decode_32(record, &value) == 0)
				log("Record ID %d = %u", record_id, value);

			if (data.data_var.record)
				mbus_data_record_free(data.data_var.record);
		}
		mbus_frame_free(reply.next);
	}

	return 0;
}

static int set_address(mbus_handle *handle, char *args)
{
	mbus_frame reply;
	int curr, next;
	char *mask;

	if (!args) {
	syntax:
		warnx("missing argument.\nUsage: set <MASK | ADDR> NEW_ADDR");
		return 1;
	}

	mask = strsep(&args, " \n\t");
	if (!args || *args == 0)
		goto syntax;

	if (!mbus_is_secondary_address(mask)) {
		curr = atoi(mask);
		if (curr < 0 || curr > 250) {
			warnx("invalid secondary address [%s], also not a primary address (0-250).", args);
			return 1;
		}
	} else {
		curr = MBUS_ADDRESS_NETWORK_LAYER;
	}

	next = atoi(args);
	if (next < 1 || next > 250) {
		warnx("invalid new primary address [%s], allowed 1-250.", args);
		return 1;
	}

	if (init_slaves(handle))
		return 1;

	if (mbus_send_ping_frame(handle, next, 0) == -1) {
		warnx("failed sending verification ping: %s", mbus_error_str());
		return 1;
	}

	if (mbus_recv_frame(handle, &reply) != MBUS_RECV_RESULT_TIMEOUT) {
		warnx("verification failed, primary address [%d] already in use.", next);
		return 1;
	}

	if (curr == MBUS_ADDRESS_NETWORK_LAYER) {
		if (secondary_select(handle, mask) == -1)
			return 1;
	}

	for (int retries = 3; retries > 0; retries--) {
		if (mbus_set_primary_address(handle, curr, next) == -1) {
			warnx("failed setting device [%s] primary address: %s", mask, mbus_error_str());
			return 1;
		}

		if (mbus_recv_frame(handle, &reply) == MBUS_RECV_RESULT_TIMEOUT) {
			if (retries > 1)
				continue;

			warnx("No reply from device [%s].", mask);
			return 1;
		}
		break;
	}

	if (mbus_frame_type(&reply) != MBUS_FRAME_TYPE_ACK) {
		warnx("invalid response from device [%s], exected ACK, got:", mask);
		mbus_frame_print(&reply);
		return 1;
	}

	dbg("primary address of device %s set to %d", mask, next);

	return 0;
}

static int set_baudrate(mbus_handle *handle, char *args)
{
	long baudrate;
	char *arg;

	if (!args) {
		warnx("missing argument.\nUsage: set [ADDR] BAUDRATE");
		return 1;
	}

	arg = strsep(&args, " \n\t");
	if (!args || *args == 0) {
		baudrate = atol(arg);
	} else {
		warnx("Setting device baudrate not supported yet.");
		return 1;
	}

	if (baudrate < 300) {
		warnx("Too low baudrate, recommeded: 300, 2400, 9600.");
		return 1;
	}
	switch (baudrate) {
	case 300:
	case 2400:
	case 9600:
		break;
	default:
		warnx("Not recommended by M-Bus standard.");
		break;
	}

	if (mbus_serial_set_baudrate(handle, baudrate) == -1) {
		warnx("Failed setting baud rate %ld on serial port %s: %s",
		      baudrate, device, mbus_error_str());
		return 1;
	}

	return 0;
}

static int toggle_debug(mbus_handle *handle, char *args)
{
	(void)args;
	debug ^= 1;

	return mbus_debug(handle, debug);
}

static int show_help(mbus_handle *handle, char *args);

struct cmd {
	char *c_cmd;
	char *c_arg;
	char *c_desc;
	int (*c_cb)(mbus_handle *, char *);
};

struct cmd cmds[] = {
	{ "address", "MASK ADDR",   "Set primary address",                    set_address   },
	{ "baud",    "[ADDR] RATE", "Set (device) baud rate [300,2400,9600]", set_baudrate  },
	{ "rate",    NULL,          NULL,                                     set_baudrate  },
	{ "request", "ADDR [ID]",   "Request data, full XML or one record",   query_device  },
	{ NULL,      NULL,          NULL,                                     NULL          },
	{ "probe",   "[MASK]",      "Secondary address scan",                 probe_devices },
	{ "scan",    NULL,          "Primary address scan",                   scan_devices  },
	{ NULL,      NULL,          NULL,                                     NULL          },
	{ "debug",   NULL,          "Toggle debug mode",                      toggle_debug  },
	{ "help",    "[CMD]",       "Display (this) menu",                    show_help     },
	{ "quit",    NULL,          "Quit",                                   quit_program  },
};

static int show_help(mbus_handle *handle, char *args)
{
	int w = 0;

	(void)handle;

	if (args) {
		size_t len = strlen(args);

		dbg("Looking for help to command '%s'", args);
		for (size_t i = 0; i < NELEMS(cmds); i++) {
			struct cmd *c = &cmds[i];

			if (!c->c_cmd)
				continue;

			if (strncmp(c->c_cmd, args, len))
				continue;

			printf("Usage:\n"
			       "\t%s %s\n\n", c->c_cmd, c->c_arg ?: "");
			printf("Description:\n"
			       "\t%s\n", c->c_desc);
			return 0;
		}

		warnx("no such command.");
		return -1;
	}

	for (size_t i = 0; i < NELEMS(cmds); i++) {
		int len;

		if (!cmds[i].c_cmd)
			continue;

		len = (int)strlen(cmds[i].c_cmd);
		if (len > w)
			w = len;
	}

	for (size_t i = 0; i < NELEMS(cmds); i++) {
		struct cmd *c = &cmds[i];

		if (!c->c_cmd) { /* separator */
			puts("");
			continue;
		}
		if (!c->c_desc)  /* alias */
			continue;

		printf("%-*s %-12s  %s\n", w, c->c_cmd, c->c_arg ?: "", c->c_desc);
	}

	return 0;
}

/* drop leading whitespace and any/all newlines */
char *chompy(char *str)
{
	char *p;

	if (!str || strlen(str) < 1)
		return NULL;

	while (*str == ' ' || *str == '\t')
		str++;

	p = str + strlen(str) - 1;
        while (p >= str && *p == '\n')
		*p-- = 0;

	return str;
}

static int readcmd(FILE *fp, mbus_handle *handle)
{
	char *cmd, *args;
	char line[42];
	size_t len;

	if (interactive) {
		fflush(fp);
		printf("\033[2K\r> ");
		fflush(stdout);
	}

	if (!(args = fgets(line, sizeof(line), fp)))
		return -1;

	args = chompy(args);
	if (!args)
		return -1;

	cmd = strsep(&args, " \n\t");
	if (args && *args == 0)
		args = NULL;

	len = strlen(cmd);
	if (len < 1)
		return 1;

	dbg("CMD: %s ARGS: %s", cmd, args ?: "");
	for (size_t i = 0; i < NELEMS(cmds); i++) {
		struct cmd *c = &cmds[i];

		if (!c->c_cmd)
			continue;

		dbg("CMP: %s vs CMD: %s", cmd, c->c_cmd);
		if (strncmp(c->c_cmd, cmd, len))
			continue;

		return c->c_cb(handle, args);
	}

	warnx("no such command. Use 'help' to list commands.");

	return 1;
}

#ifndef __ZEPHYR__
static void sigcb(int signo)
{
	running = 0;
}

static int usage(int rc)
{
	fprintf(stderr,
		"Usage: %s [-d] [-b RATE] DEVICE\n"
		"\n"
		"Options:\n"
		" -b RATE    Set baudrate: 300, 2400, 9600, default: 2400\n"
		" -d         Enable debug messages\n"
		" -f FILE    Execute commands from file and then exit\n"
		"Arguments:\n"
		" DEVICE     Serial port/pty to use\n"
		"\n"
		"Copyright (c) 2022  Addiva Elektronik AB\n", arg0);
	return rc;
}
#endif

int main(int argc, char **argv)
{
	mbus_handle *handle;
	char *file = NULL;
	char *rate = NULL;
	FILE *fp = stdin;
#ifndef __ZEPHYR__
	int c;

	arg0 = argv[0];
	signal(SIGINT, sigcb);
	signal(SIGHUP, sigcb);
	signal(SIGTERM, sigcb);

	while ((c = getopt(argc, argv, "b:df:")) != EOF) {
		switch (c) {
		case 'b':
			rate = optarg;
			break;
		case 'd':
			debug = 1;
			break;
		case 'f':
			file = optarg;
			break;
		default:
			return usage(0);
		}
	}

	if (optind >= argc)
		return usage(1);
	device = argv[optind++];
#else
	device = uart0;
#endif
	if (file) {
		fp = fopen(file, "r");
		if (!fp)
			err(1, "failed opening %s for reading", file);
	}
	interactive = isatty(fileno(fp));

	handle = mbus_context_serial(device);
	if (!handle) {
		warnx("Failed initializing M-Bus context: %s", mbus_error_str());
		return 1;
	}

	if (mbus_connect(handle) == -1)
		errx(1, "Failed opening serial port %s: %s", device, mbus_error_str());

	if (rate && set_baudrate(handle, rate))
		goto error;

	mbus_debug(handle, debug);
	while (running) {
		if (readcmd(fp, handle)) {
			if (!interactive && feof(fp))
				break;
		}
	}

error:
	if (file)
		fclose(fp);
	mbus_disconnect(handle);
	mbus_context_free(handle);

	return 0;
}
