#include "websocket-api.h"
#include "smooth-media-source.h"

#include <obs-module.h>
#include <util/dstr.h>
#include "obs-websocket-api.h"

static obs_websocket_vendor vendor = NULL;

/* ──────────────────────────────────────────────
 *  Helper: find a smooth_media_source by source name
 * ────────────────────────────────────────────── */

struct source_find_ctx {
	const char *name;
	obs_source_t *source;
};

static bool find_source_by_name(void *data, obs_source_t *source)
{
	struct source_find_ctx *ctx = data;
	const char *id = obs_source_get_unversioned_id(source);
	const char *name = obs_source_get_name(source);

	if (id && strcmp(id, "smooth_media_source") == 0 &&
	    name && strcmp(name, ctx->name) == 0) {
		ctx->source = obs_source_get_ref(source);
		return false; /* stop enumeration */
	}
	return true;
}

static obs_source_t *find_smooth_source(const char *name)
{
	if (!name || !*name)
		return NULL;

	struct source_find_ctx ctx = {name, NULL};
	obs_enum_sources(find_source_by_name, &ctx);
	return ctx.source;
}

/* ──────────────────────────────────────────────
 *  Request: SetStreamURL
 *  { "sourceName": "...", "url": "rtmp://..." }
 * ────────────────────────────────────────────── */

static void on_set_stream_url(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);

	const char *source_name = obs_data_get_string(request_data, "sourceName");
	const char *url = obs_data_get_string(request_data, "url");

	if (!source_name || !*source_name) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "Missing 'sourceName' parameter");
		return;
	}

	if (!url) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "Missing 'url' parameter");
		return;
	}

	obs_source_t *source = find_smooth_source(source_name);
	if (!source) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "Source not found");
		return;
	}

	/* Update the source settings and trigger reconnect */
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_string(settings, "input", url);
	obs_source_update(source, settings);
	obs_data_release(settings);
	obs_source_release(source);

	blog(LOG_INFO, "[obs-smooth-media] WebSocket: SetStreamURL '%s' -> %s",
	     source_name, url);

	obs_data_set_bool(response_data, "success", true);
	obs_data_set_string(response_data, "url", url);
}

/* ──────────────────────────────────────────────
 *  Request: GetStreamURL
 *  { "sourceName": "..." }
 * ────────────────────────────────────────────── */

static void on_get_stream_url(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);

	const char *source_name = obs_data_get_string(request_data, "sourceName");

	if (!source_name || !*source_name) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "Missing 'sourceName' parameter");
		return;
	}

	obs_source_t *source = find_smooth_source(source_name);
	if (!source) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "Source not found");
		return;
	}

	obs_data_t *settings = obs_source_get_settings(source);
	const char *url = obs_data_get_string(settings, "input");

	obs_data_set_bool(response_data, "success", true);
	obs_data_set_string(response_data, "url", url ? url : "");
	obs_data_release(settings);
	obs_source_release(source);
}

/* ──────────────────────────────────────────────
 *  Request: GetStatus
 *  { "sourceName": "..." }
 * ────────────────────────────────────────────── */

static void on_get_status(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(priv_data);

	const char *source_name = obs_data_get_string(request_data, "sourceName");

	if (!source_name || !*source_name) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "Missing 'sourceName' parameter");
		return;
	}

	obs_source_t *source = find_smooth_source(source_name);
	if (!source) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "Source not found");
		return;
	}

	struct smooth_media_source *s = obs_obj_get_data(source);
	if (!s) {
		obs_data_set_bool(response_data, "success", false);
		obs_data_set_string(response_data, "error", "Source data unavailable");
		obs_source_release(source);
		return;
	}

	const char *state_str = "unknown";
	switch (s->state) {
	case OBS_MEDIA_STATE_NONE:    state_str = "none"; break;
	case OBS_MEDIA_STATE_PLAYING: state_str = "playing"; break;
	case OBS_MEDIA_STATE_OPENING: state_str = "opening"; break;
	case OBS_MEDIA_STATE_BUFFERING: state_str = "buffering"; break;
	case OBS_MEDIA_STATE_PAUSED:  state_str = "paused"; break;
	case OBS_MEDIA_STATE_STOPPED: state_str = "stopped"; break;
	case OBS_MEDIA_STATE_ENDED:   state_str = "ended"; break;
	case OBS_MEDIA_STATE_ERROR:   state_str = "error"; break;
	}

	obs_data_set_bool(response_data, "success", true);
	obs_data_set_string(response_data, "state", state_str);
	obs_data_set_bool(response_data, "active", s->active);
	obs_data_set_bool(response_data, "reconnecting", s->reconnecting);
	obs_data_set_string(response_data, "url", s->url ? s->url : "");
	obs_data_set_int(response_data, "audioFramesOut", (long long)s->audio_frames_out);
	obs_data_set_int(response_data, "videoFramesOut", (long long)s->video_frames_out);

	/* A/V wall offset in ms */
	int64_t av_wall = 0;
	if (s->audio_out_ts && s->video_out_ts)
		av_wall = (s->audio_out_ts - s->video_out_ts) / 1000000;
	obs_data_set_int(response_data, "avOffsetMs", (long long)av_wall);

	obs_source_release(source);
}

/* ──────────────────────────────────────────────
 *  Request: ListSources
 *  (no parameters)
 * ────────────────────────────────────────────── */

struct source_list_ctx {
	obs_data_array_t *array;
};

static bool list_source_cb(void *data, obs_source_t *source)
{
	struct source_list_ctx *ctx = data;
	const char *id = obs_source_get_unversioned_id(source);

	if (id && strcmp(id, "smooth_media_source") == 0) {
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", obs_source_get_name(source));

		obs_data_t *settings = obs_source_get_settings(source);
		const char *url = obs_data_get_string(settings, "input");
		obs_data_set_string(item, "url", url ? url : "");
		obs_data_release(settings);

		obs_data_array_push_back(ctx->array, item);
		obs_data_release(item);
	}
	return true;
}

static void on_list_sources(obs_data_t *request_data, obs_data_t *response_data, void *priv_data)
{
	UNUSED_PARAMETER(request_data);
	UNUSED_PARAMETER(priv_data);

	obs_data_array_t *array = obs_data_array_create();
	struct source_list_ctx ctx = {array};
	obs_enum_sources(list_source_cb, &ctx);

	obs_data_set_bool(response_data, "success", true);
	obs_data_set_array(response_data, "sources", array);
	obs_data_array_release(array);
}

/* ──────────────────────────────────────────────
 *  Public API
 * ────────────────────────────────────────────── */

void smooth_media_websocket_init(void)
{
	vendor = obs_websocket_register_vendor("obs-smooth-media");
	if (!vendor) {
		blog(LOG_WARNING, "[obs-smooth-media] WebSocket vendor registration failed "
		     "(obs-websocket may not be installed)");
		return;
	}

	bool ok = true;
	ok = obs_websocket_vendor_register_request(vendor, "SetStreamURL", on_set_stream_url, NULL) && ok;
	ok = obs_websocket_vendor_register_request(vendor, "GetStreamURL", on_get_stream_url, NULL) && ok;
	ok = obs_websocket_vendor_register_request(vendor, "GetStatus", on_get_status, NULL) && ok;
	ok = obs_websocket_vendor_register_request(vendor, "ListSources", on_list_sources, NULL) && ok;

	if (!ok)
		blog(LOG_WARNING, "[obs-smooth-media] Some WebSocket requests failed to register");

	blog(LOG_INFO, "[obs-smooth-media] WebSocket vendor API registered:");
	blog(LOG_INFO, "[obs-smooth-media]   - SetStreamURL");
	blog(LOG_INFO, "[obs-smooth-media]   - GetStreamURL");
	blog(LOG_INFO, "[obs-smooth-media]   - GetStatus");
	blog(LOG_INFO, "[obs-smooth-media]   - ListSources");
}
