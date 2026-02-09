#include <obs-module.h>
#include "smooth-media-source.h"

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
	     "1.2.0");
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[obs-smooth-media] Plugin unloaded");
}
