#include <obs-module.h>
#include "smooth-media-source.h"
#include "stream-decoder.h"
#include "websocket-api.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-smooth-media", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Smooth Media Source — stutter-free RTMP/SRT playback with "
	       "clock drift correction and audio jitter buffering";
}

bool obs_module_load(void)
{
	obs_register_source(&smooth_media_source_info);
	blog(LOG_INFO, "[obs-smooth-media] Plugin loaded (v%s)",
	     SMOOTH_MEDIA_VERSION);
	stream_decoder_log_protocols();
	return true;
}

void obs_module_post_load(void)
{
	smooth_media_websocket_init();
}

void obs_module_unload(void)
{
	stream_decoder_global_cleanup();
	blog(LOG_INFO, "[obs-smooth-media] Plugin unloaded");
}
