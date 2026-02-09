/*
obs-websocket
Copyright (C) 2016-2021 Stephane Lepin <stephane.lepin@gmail.com>
Copyright (C) 2020-2022 Kyle Manning <tt2468@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <obs.h>
#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *obs_websocket_vendor;
typedef void (*obs_websocket_request_callback_function)(obs_data_t *, obs_data_t *, void *);
typedef void (*obs_websocket_event_callback_function)(uint64_t, const char *, const char *, void *);

struct obs_websocket_request_response {
	unsigned int status_code;
	char *comment;
	char *response_data;
};

/* ==================== INTERNAL DEFINITIONS ==================== */

struct obs_websocket_request_callback {
	obs_websocket_request_callback_function callback;
	void *priv_data;
};

struct obs_websocket_event_callback {
	obs_websocket_event_callback_function callback;
	void *priv_data;
};

static proc_handler_t *_ph;

/* ==================== INTERNAL API FUNCTIONS ==================== */

static inline proc_handler_t *obs_websocket_get_ph(void)
{
	proc_handler_t *global_ph = obs_get_proc_handler();
	assert(global_ph != NULL);

	calldata_t cd = {0, 0, 0, 0};
	if (!proc_handler_call(global_ph, "obs_websocket_api_get_ph", &cd))
		blog(LOG_DEBUG, "Unable to fetch obs-websocket proc handler object. obs-websocket not installed?");
	proc_handler_t *ret = (proc_handler_t *)calldata_ptr(&cd, "ph");
	calldata_free(&cd);

	return ret;
}

static inline bool obs_websocket_ensure_ph(void)
{
	if (!_ph)
		_ph = obs_websocket_get_ph();
	return _ph != NULL;
}

static inline bool obs_websocket_vendor_run_simple_proc(obs_websocket_vendor vendor, const char *proc_name, calldata_t *cd)
{
	if (!obs_websocket_ensure_ph())
		return false;

	if (!vendor || !proc_name || !strlen(proc_name) || !cd)
		return false;

	calldata_set_ptr(cd, "vendor", vendor);

	proc_handler_call(_ph, proc_name, cd);
	return calldata_bool(cd, "success");
}

/* ==================== VENDOR API FUNCTIONS ==================== */

/* ALWAYS CALL ONLY VIA obs_module_post_load() CALLBACK! */
static inline obs_websocket_vendor obs_websocket_register_vendor(const char *vendor_name)
{
	if (!obs_websocket_ensure_ph())
		return NULL;

	calldata_t cd = {0, 0, 0, 0};
	calldata_set_string(&cd, "name", vendor_name);

	proc_handler_call(_ph, "vendor_register", &cd);
	obs_websocket_vendor ret = calldata_ptr(&cd, "vendor");
	calldata_free(&cd);

	return ret;
}

static inline bool obs_websocket_vendor_register_request(obs_websocket_vendor vendor, const char *request_type,
							 obs_websocket_request_callback_function request_callback, void *priv_data)
{
	struct obs_websocket_request_callback cb = {request_callback, priv_data};

	calldata_t cd = {0, 0, 0, 0};
	calldata_set_string(&cd, "type", request_type);
	calldata_set_ptr(&cd, "callback", &cb);

	bool success = obs_websocket_vendor_run_simple_proc(vendor, "vendor_request_register", &cd);
	calldata_free(&cd);

	return success;
}

static inline bool obs_websocket_vendor_emit_event(obs_websocket_vendor vendor, const char *event_name, obs_data_t *event_data)
{
	calldata_t cd = {0, 0, 0, 0};
	calldata_set_string(&cd, "type", event_name);
	calldata_set_ptr(&cd, "data", (void *)event_data);

	bool success = obs_websocket_vendor_run_simple_proc(vendor, "vendor_event_emit", &cd);
	calldata_free(&cd);

	return success;
}

#ifdef __cplusplus
}
#endif
