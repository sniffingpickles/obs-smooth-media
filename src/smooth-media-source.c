#include "smooth-media-source.h"

#include <util/dstr.h>
#include <util/platform.h>
#include <math.h>

#define PLUGIN_LOG_PREFIX "[Smooth Media Source '%s']: "
#define SM_LOG(level, format, ...) \
	blog(level, PLUGIN_LOG_PREFIX format, \
	     obs_source_get_name(s->source), ##__VA_ARGS__)

/* Hardcoded tuning constants — these don't need user-facing sliders.
 * They've been empirically validated across RTMP/SRT streams. */
#define NETWORK_BUFFER_MB   2
#define JITTER_BUFFER_MS    80
#define MAX_BUFFER_MS       500
#define SR_WARMUP_NS        (5000000000LL) /* 5s: skip rate correction while clock tracker settles */
#define SR_HOLD_TIME_NS     (3000000000LL) /* 3s: rate must stay outside deadzone this long before adjusting */

/* Forward declarations */
static void smooth_media_update(void *data, obs_data_t *settings);

/* ──────────────────────────────────────────────
 *  Format conversion helpers (AVFormat → OBS)
 * ────────────────────────────────────────────── */

static inline enum video_format av_to_obs_video_format(int f)
{
	switch (f) {
	case AV_PIX_FMT_YUV420P:   return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUYV422:   return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_YUV422P:   return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:   return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_UYVY422:   return VIDEO_FORMAT_UYVY;
	case AV_PIX_FMT_YVYU422:   return VIDEO_FORMAT_YVYU;
	case AV_PIX_FMT_NV12:      return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_RGBA:      return VIDEO_FORMAT_RGBA;
	case AV_PIX_FMT_BGRA:      return VIDEO_FORMAT_BGRA;
	case AV_PIX_FMT_YUVA420P:  return VIDEO_FORMAT_I40A;
	case AV_PIX_FMT_YUV420P10LE: return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_YUV422P10LE: return VIDEO_FORMAT_I210;
	case AV_PIX_FMT_YUVA422P:  return VIDEO_FORMAT_I42A;
	case AV_PIX_FMT_YUVA444P:  return VIDEO_FORMAT_YUVA;
	case AV_PIX_FMT_BGR0:      return VIDEO_FORMAT_BGRX;
	case AV_PIX_FMT_P010LE:    return VIDEO_FORMAT_P010;
	default: return VIDEO_FORMAT_NONE;
	}
}

static inline enum audio_format av_to_obs_audio_format(int f)
{
	switch (f) {
	case AV_SAMPLE_FMT_U8:    return AUDIO_FORMAT_U8BIT;
	case AV_SAMPLE_FMT_S16:   return AUDIO_FORMAT_16BIT;
	case AV_SAMPLE_FMT_S32:   return AUDIO_FORMAT_32BIT;
	case AV_SAMPLE_FMT_FLT:   return AUDIO_FORMAT_FLOAT;
	case AV_SAMPLE_FMT_U8P:   return AUDIO_FORMAT_U8BIT_PLANAR;
	case AV_SAMPLE_FMT_S16P:  return AUDIO_FORMAT_16BIT_PLANAR;
	case AV_SAMPLE_FMT_S32P:  return AUDIO_FORMAT_32BIT_PLANAR;
	case AV_SAMPLE_FMT_FLTP:  return AUDIO_FORMAT_FLOAT_PLANAR;
	default: return AUDIO_FORMAT_UNKNOWN;
	}
}

static inline enum speaker_layout channels_to_speakers(uint32_t ch)
{
	switch (ch) {
	case 1: return SPEAKERS_MONO;
	case 2: return SPEAKERS_STEREO;
	case 3: return SPEAKERS_2POINT1;
	case 4: return SPEAKERS_4POINT0;
	case 5: return SPEAKERS_4POINT1;
	case 6: return SPEAKERS_5POINT1;
	case 8: return SPEAKERS_7POINT1;
	default: return SPEAKERS_UNKNOWN;
	}
}

static inline enum video_colorspace av_to_obs_colorspace(int cs, int trc,
							 int primaries)
{
	switch (cs) {
	case AVCOL_SPC_BT709:
		return (trc == AVCOL_TRC_IEC61966_2_1) ? VIDEO_CS_SRGB
						       : VIDEO_CS_709;
	case AVCOL_SPC_FCC:
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_SMPTE240M:
		return VIDEO_CS_601;
	case AVCOL_SPC_BT2020_NCL:
		return (trc == AVCOL_TRC_ARIB_STD_B67) ? VIDEO_CS_2100_HLG
						       : VIDEO_CS_2100_PQ;
	default:
		return (primaries == AVCOL_PRI_BT2020)
			       ? ((trc == AVCOL_TRC_ARIB_STD_B67)
					  ? VIDEO_CS_2100_HLG
					  : VIDEO_CS_2100_PQ)
			       : VIDEO_CS_DEFAULT;
	}
}

/* ──────────────────────────────────────────────
 *  Decoder callbacks — receive decoded frames
 * ────────────────────────────────────────────── */

static void on_video_frame(void *opaque, struct decoded_video_frame *vf)
{
	struct smooth_media_source *s = opaque;

	if (!vf->width || !vf->height)
		return;

	/* Skip until first keyframe */
	if (!s->got_first_keyframe) {
		if (!vf->keyframe)
			return;
		s->got_first_keyframe = true;
	}

	/* Record PTS for clock tracking */
	int64_t wall_now = (int64_t)os_gettime_ns();
	clock_tracker_record(&s->clock, vf->pts_ns, wall_now);

	/* Set anchor on first video */
	if (!s->first_video) {
		s->first_video = true;
		s->first_video_pts = vf->pts_ns;
		SM_LOG(LOG_INFO,
		       "First video frame: %dx%d fmt=%d pts=%lldms",
		       vf->width, vf->height, vf->format,
		       (long long)(vf->pts_ns / 1000000));
	}

	/* Compute output timestamp using PTS-delta stepping.
	 * Video is always the timing master — same logic regardless
	 * of sync_pts setting. PTS deltas give smooth frame spacing;
	 * asymmetric drift correction keeps timestamps near wall clock.
	 *
	 * Video is offset forward by the jitter buffer depth so OBS
	 * holds the frame before displaying it. This compensates for
	 * the delay audio experiences sitting in the jitter buffer. */
	int64_t buf_offset = s->audio_buf.min_buffer_ns;
	int64_t out_ts;

	if (s->video_frames_out == 0) {
		out_ts = wall_now + buf_offset;
		s->video_next_ts = wall_now + buf_offset;
		s->prev_video_pts = vf->pts_ns;
	} else {
		int64_t pts_delta = vf->pts_ns - s->prev_video_pts;
		s->prev_video_pts = vf->pts_ns;

		if (pts_delta > 0 && pts_delta < 500000000LL) {
			s->video_next_ts += pts_delta;
		} else {
			s->video_next_ts = wall_now + buf_offset;
		}

		/* Asymmetric drift correction toward wall + buffer offset.
		 * BEHIND: 1% when >100ms, 0.1% steady-state.
		 * AHEAD: aggressive 10% to snap back during bursts.
		 * Hard clamp at target + 20ms. */
		int64_t drift_target = wall_now + buf_offset;
		int64_t drift_error = drift_target - s->video_next_ts;
		if (drift_error < 0) {
			s->video_next_ts += drift_error / 10;
		} else if (drift_error > 100000000LL) {
			s->video_next_ts += drift_error / 100;
		} else {
			s->video_next_ts += drift_error / 1000;
		}

		int64_t max_ahead = drift_target + 20000000LL;
		if (s->video_next_ts > max_ahead)
			s->video_next_ts = max_ahead;

		out_ts = s->video_next_ts;
	}
	s->video_out_ts = out_ts;
	s->video_frames_out++;

	/* Build OBS frame */
	struct obs_source_frame frame = {0};
	enum video_format obs_fmt = av_to_obs_video_format(vf->format);
	enum video_colorspace space = av_to_obs_colorspace(
		vf->colorspace, vf->color_trc, vf->color_primaries);
	enum video_range_type range = (vf->color_range == AVCOL_RANGE_JPEG)
					      ? VIDEO_RANGE_FULL
					      : VIDEO_RANGE_DEFAULT;

	for (int i = 0; i < 4; i++) {
		frame.data[i] = vf->data[i];
		frame.linesize[i] = abs(vf->linesize[i]);
	}

	bool flip = vf->linesize[0] < 0 && vf->linesize[1] == 0;
	if (flip)
		frame.data[0] -= frame.linesize[0] * ((size_t)vf->height - 1);

	frame.format = obs_fmt;
	frame.width = vf->width;
	frame.height = vf->height;
	frame.timestamp = out_ts;
	frame.flip = flip;
	frame.full_range = (range == VIDEO_RANGE_FULL);
	frame.max_luminance = vf->max_luminance;

	switch (vf->color_trc) {
	case AVCOL_TRC_BT709:
	case AVCOL_TRC_GAMMA22:
	case AVCOL_TRC_GAMMA28:
	case AVCOL_TRC_SMPTE170M:
	case AVCOL_TRC_SMPTE240M:
	case AVCOL_TRC_IEC61966_2_1:
		frame.trc = VIDEO_TRC_SRGB;
		break;
	case AVCOL_TRC_SMPTE2084:
		frame.trc = VIDEO_TRC_PQ;
		break;
	case AVCOL_TRC_ARIB_STD_B67:
		frame.trc = VIDEO_TRC_HLG;
		break;
	default:
		frame.trc = VIDEO_TRC_DEFAULT;
	}

	video_format_get_parameters_for_format(
		space, range, obs_fmt,
		frame.color_matrix, frame.color_range_min,
		frame.color_range_max);

	if (frame.format != VIDEO_FORMAT_NONE)
		obs_source_output_video(s->source, &frame);
}

static void on_audio_frame(void *opaque, struct decoded_audio_frame *af)
{
	struct smooth_media_source *s = opaque;

	if (af->frames == 0 || af->sample_rate == 0)
		return;

	/* Record PTS for clock tracking */
	int64_t wall_now = (int64_t)os_gettime_ns();
	clock_tracker_record(&s->clock, af->pts_ns, wall_now);

	/* Set anchor on first audio */
	if (!s->first_audio) {
		s->first_audio = true;
		s->first_audio_pts = af->pts_ns;
		SM_LOG(LOG_INFO,
		       "First audio frame: %uHz %uch %u samples pts=%lldms",
		       af->sample_rate, af->channels, af->frames,
		       (long long)(af->pts_ns / 1000000));
	}

	/* Push into the jitter buffer instead of outputting directly.
	 * Audio is drained at a steady pace in smooth_media_tick(),
	 * which prevents bursty network delivery from causing OBS
	 * to accumulate audio buffering. */
	enum audio_format obs_fmt = av_to_obs_audio_format(af->format);
	if (obs_fmt == AUDIO_FORMAT_UNKNOWN)
		return;

	bool was_primed = audio_buffer_is_ready(&s->audio_buf);

	audio_buffer_push(&s->audio_buf,
			  (const uint8_t *const *)af->data,
			  af->data_size, af->frames,
			  af->sample_rate, af->channels,
			  (int)obs_fmt,
			  (int)channels_to_speakers(af->channels),
			  af->pts_ns);

	if (!was_primed && audio_buffer_is_ready(&s->audio_buf)) {
		SM_LOG(LOG_INFO,
		       "Jitter buffer primed (%lldms buffered)",
		       (long long)(audio_buffer_level_ns(&s->audio_buf) / 1000000));
	}

	/* Log if the buffer had to drop frames (overflow).
	 * Batch log at most once per second to avoid spam during
	 * initial bursts (which can produce 80+ drops instantly). */
	if (s->audio_buf.frames_dropped > s->last_drop_count) {
		s->pending_drop_count +=
			s->audio_buf.frames_dropped - s->last_drop_count;
		s->last_drop_count = s->audio_buf.frames_dropped;

		if (wall_now - s->last_overflow_log_time >=
		    1000000000LL) {
			SM_LOG(LOG_WARNING,
			       "Audio buffer overflow: dropped %llu frame(s) "
			       "(total dropped: %llu, buffered: %lldms)",
			       (unsigned long long)s->pending_drop_count,
			       (unsigned long long)
				       s->audio_buf.frames_dropped,
			       (long long)(audio_buffer_level_ns(
						  &s->audio_buf) /
					  1000000));
			s->pending_drop_count = 0;
			s->last_overflow_log_time = wall_now;
		}
	}
}

static void on_stream_stopped(void *opaque)
{
	(void)opaque;
}

/* ──────────────────────────────────────────────
 *  Media thread — runs the decoder loop
 * ────────────────────────────────────────────── */

static void *media_thread_func(void *data)
{
	struct smooth_media_source *s = data;

	os_set_thread_name("smooth_media_thread");

	struct stream_decoder_info info = {
		.url = s->url,
		.format_name = s->input_format,
		.ffmpeg_options = s->ffmpeg_options,
		.buffering_bytes = NETWORK_BUFFER_MB * 1024 * 1024,
		.hardware_decoding = s->hw_decode,
		.opaque = s,
		.video_cb = on_video_frame,
		.audio_cb = on_audio_frame,
		.stop_cb = on_stream_stopped,
	};

	s->decoder = stream_decoder_create(&info);
	if (!s->decoder) {
		if (s->reconnect_attempts == 0)
			SM_LOG(LOG_WARNING, "Failed to open stream: %s", s->url);
		s->active = false;

		pthread_mutex_lock(&s->state_mutex);
		s->state = OBS_MEDIA_STATE_ENDED;
		pthread_mutex_unlock(&s->state_mutex);

		obs_source_media_ended(s->source);
		return NULL;
	}

	if (s->reconnect_attempts > 0)
		SM_LOG(LOG_INFO, "Reconnected after %u attempts: %s",
		       s->reconnect_attempts, s->url);
	else
		SM_LOG(LOG_INFO, "Stream opened: %s", s->url);
	s->reconnect_attempts = 0;

	/* Main decode loop.
	 * Unlike OBS's built-in media source, we do NOT drive timing here.
	 * We just decode as fast as data arrives and push into the jitter
	 * buffer. The clock tracker and jitter buffer handle pacing. */
	while (s->active && !s->kill) {
		if (!stream_decoder_read_next(s->decoder)) {
			/* EOF or fatal error */
			break;
		}
	}

	stream_decoder_destroy(s->decoder);
	s->decoder = NULL;
	s->active = false;

	/* Signal end of media */
	obs_source_output_video(s->source, NULL);

	pthread_mutex_lock(&s->state_mutex);
	if (s->state != OBS_MEDIA_STATE_STOPPED) {
		s->state = OBS_MEDIA_STATE_ENDED;
		obs_source_media_ended(s->source);
	}
	pthread_mutex_unlock(&s->state_mutex);

	return NULL;
}

/* ──────────────────────────────────────────────
 *  Source lifecycle
 * ────────────────────────────────────────────── */

static void stop_media_thread(struct smooth_media_source *s)
{
	if (!s->media_thread_valid)
		return;

	s->kill = true;
	s->active = false;

	if (s->decoder)
		stream_decoder_request_stop(s->decoder);

	pthread_join(s->media_thread, NULL);
	s->media_thread_valid = false;
	s->kill = false;
}

static void start_media(struct smooth_media_source *s)
{
	stop_media_thread(s);

	if (!s->url || !*s->url)
		return;

	/* Reset all state */
	audio_buffer_reset(&s->audio_buf);
	clock_tracker_reset(&s->clock);
	s->first_audio = false;
	s->first_video = false;
	s->got_first_keyframe = false;
	s->audio_out_ts = 0;
	s->video_out_ts = 0;
	s->audio_frames_out = 0;
	s->video_frames_out = 0;
	s->last_drop_count = 0;
	s->last_overflow_log_time = 0;
	s->pending_drop_count = 0;
	s->last_diag_time = 0;
	s->stream_start_time = (int64_t)os_gettime_ns();
	s->sr_hold_start = 0;
	s->last_audio_pop_time = 0;
	s->audio_frame_dur_ns = 0;
	s->prev_video_pts = 0;
	s->video_next_ts = 0;
	s->prev_audio_pts = 0;
	s->audio_next_ts = 0;
	s->active = true;
	s->kill = false;

	/* Configure jitter buffer (hardcoded — proven values) */
	s->audio_buf.min_buffer_ns = JITTER_BUFFER_MS * 1000000LL;
	s->audio_buf.max_buffer_ns = MAX_BUFFER_MS * 1000000LL;

	pthread_mutex_lock(&s->state_mutex);
	s->state = OBS_MEDIA_STATE_PLAYING;
	pthread_mutex_unlock(&s->state_mutex);

	obs_source_media_started(s->source);

	if (pthread_create(&s->media_thread, NULL, media_thread_func, s) != 0) {
		SM_LOG(LOG_ERROR, "Failed to create media thread");
		s->active = false;
		return;
	}
	s->media_thread_valid = true;
}

/* ──────────────────────────────────────────────
 *  Reconnection
 * ────────────────────────────────────────────── */

static void stop_reconnect_thread(struct smooth_media_source *s)
{
	pthread_mutex_lock(&s->reconnect_mutex);
	if (s->reconnect_thread_valid) {
		os_event_signal(s->reconnect_stop_event);
		pthread_join(s->reconnect_thread, NULL);
		s->reconnect_thread_valid = false;
		s->reconnecting = false;
		os_event_reset(s->reconnect_stop_event);
	}
	pthread_mutex_unlock(&s->reconnect_mutex);
}

static void *reconnect_thread_func(void *data)
{
	struct smooth_media_source *s = data;

	int ret = os_event_timedwait(s->reconnect_stop_event,
				     s->reconnect_delay_sec * 1000);
	if (ret == 0)
		return NULL;

	s->reconnect_attempts++;
	if (s->reconnect_attempts == 1 || (s->reconnect_attempts % 10) == 0)
		SM_LOG(LOG_INFO, "Reconnect attempt #%u...",
		       s->reconnect_attempts);
	start_media(s);

	/* Clear reconnecting flag so smooth_media_tick can schedule
	 * another reconnect if this attempt also fails. */
	pthread_mutex_lock(&s->reconnect_mutex);
	s->reconnecting = false;
	pthread_mutex_unlock(&s->reconnect_mutex);

	return NULL;
}

static void schedule_reconnect(struct smooth_media_source *s)
{
	pthread_mutex_lock(&s->reconnect_mutex);
	if (!s->reconnecting) {
		s->reconnecting = true;
		if (s->reconnect_attempts == 0)
			SM_LOG(LOG_WARNING, "Stream disconnected. Reconnecting every %d seconds...",
			       s->reconnect_delay_sec);
	}

	if (s->reconnect_thread_valid) {
		os_event_signal(s->reconnect_stop_event);
		pthread_join(s->reconnect_thread, NULL);
		s->reconnect_thread_valid = false;
		os_event_reset(s->reconnect_stop_event);
	}

	if (pthread_create(&s->reconnect_thread, NULL,
			   reconnect_thread_func, s) == 0) {
		s->reconnect_thread_valid = true;
	}
	pthread_mutex_unlock(&s->reconnect_mutex);
}

/* ──────────────────────────────────────────────
 *  OBS source callbacks
 * ────────────────────────────────────────────── */

static const char *smooth_media_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Smooth Media Source";
}

static void smooth_media_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "reconnect_delay_sec", 10);
	obs_data_set_default_bool(settings, "hw_decode", false);
	obs_data_set_default_bool(settings, "sync_pts", false);
	obs_data_set_default_bool(settings, "close_when_inactive", true);
}

static obs_properties_t *smooth_media_get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_properties_add_text(props, "input",
				"Stream URL (RTMP/SRT/RIST)",
				OBS_TEXT_DEFAULT);

	obs_properties_add_text(props, "input_format",
				"Input Format (optional)",
				OBS_TEXT_DEFAULT);

	obs_property_t *p;

	p = obs_properties_add_int_slider(props, "reconnect_delay_sec",
					  "Reconnect Delay",
					  1, 60, 1);
	obs_property_int_set_suffix(p, " s");

	obs_properties_add_bool(props, "hw_decode",
				"Hardware Decoding");

	obs_properties_add_bool(props, "sync_pts",
				"Sync A/V via PTS (experimental)");

	obs_properties_add_bool(props, "close_when_inactive",
				"Close when inactive (stop when not visible)");

	obs_properties_add_text(props, "ffmpeg_options",
				"FFmpeg Options",
				OBS_TEXT_DEFAULT);

	return props;
}

static void *smooth_media_create(obs_data_t *settings, obs_source_t *source)
{
	struct smooth_media_source *s =
		bzalloc(sizeof(struct smooth_media_source));
	s->source = source;

	if (os_event_init(&s->reconnect_stop_event, OS_EVENT_TYPE_MANUAL)) {
		bfree(s);
		return NULL;
	}
	if (pthread_mutex_init(&s->reconnect_mutex, NULL)) {
		os_event_destroy(s->reconnect_stop_event);
		bfree(s);
		return NULL;
	}
	pthread_mutex_init(&s->state_mutex, NULL);

	audio_buffer_init(&s->audio_buf);
	clock_tracker_init(&s->clock);

	/* Apply settings — this will also start the stream */
	smooth_media_update(s, settings);
	return s;
}

static void smooth_media_destroy(void *data)
{
	struct smooth_media_source *s = data;

	stop_reconnect_thread(s);
	stop_media_thread(s);

	audio_buffer_free(&s->audio_buf);

	pthread_mutex_destroy(&s->reconnect_mutex);
	pthread_mutex_destroy(&s->state_mutex);
	os_event_destroy(s->reconnect_stop_event);

	bfree(s->url);
	bfree(s->input_format);
	bfree(s->ffmpeg_options);
	bfree(s);
}

static void smooth_media_update(void *data, obs_data_t *settings)
{
	struct smooth_media_source *s = data;

	stop_reconnect_thread(s);

	const char *url = obs_data_get_string(settings, "input");
	const char *fmt = obs_data_get_string(settings, "input_format");
	const char *opts = obs_data_get_string(settings, "ffmpeg_options");

	bfree(s->url);
	bfree(s->input_format);
	bfree(s->ffmpeg_options);

	s->url = url ? bstrdup(url) : NULL;
	s->input_format = (fmt && *fmt) ? bstrdup(fmt) : NULL;
	s->ffmpeg_options = (opts && *opts) ? bstrdup(opts) : NULL;
	s->hw_decode = obs_data_get_bool(settings, "hw_decode");
	s->sync_pts = obs_data_get_bool(settings, "sync_pts");
	s->close_when_inactive = obs_data_get_bool(settings, "close_when_inactive");
	s->reconnect_delay_sec = (int)obs_data_get_int(settings, "reconnect_delay_sec");

	if (s->reconnect_delay_sec < 1)
		s->reconnect_delay_sec = 10;

	/* Restart stream with new settings.
	 * If close_when_inactive, only start when visible. */
	if (s->url && *s->url) {
		if (!s->close_when_inactive ||
		    obs_source_showing(s->source))
			start_media(s);
	} else {
		stop_media_thread(s);
	}
}

static void smooth_media_activate(void *data)
{
	struct smooth_media_source *s = data;
	if (!s->close_when_inactive && s->url && *s->url && !s->active)
		start_media(s);
}

static void smooth_media_deactivate(void *data)
{
	struct smooth_media_source *s = data;
	if (!s->close_when_inactive)
		return;
	stop_reconnect_thread(s);
	stop_media_thread(s);
	obs_source_output_video(s->source, NULL);
}

static void smooth_media_show(void *data)
{
	struct smooth_media_source *s = data;
	if (s->close_when_inactive && s->url && *s->url && !s->active)
		start_media(s);
}

static void smooth_media_hide(void *data)
{
	struct smooth_media_source *s = data;
	if (!s->close_when_inactive)
		return;
	stop_reconnect_thread(s);
	stop_media_thread(s);
	obs_source_output_video(s->source, NULL);
}

static void smooth_media_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct smooth_media_source *s = data;

	/* ── Drain audio from jitter buffer at a steady pace ──
	 *
	 * Previously, audio was popped 1:1 with push in on_audio_frame.
	 * During network bursts many frames arrived in a few ms, all got
	 * popped and output immediately, and OBS accumulated hundreds of
	 * ms of audio buffering (which never decreases).
	 *
	 * Now: on_audio_frame only pushes.  video_tick (called by OBS at
	 * render fps, typically 60 Hz) pops at most a few frames per
	 * call, gated by real elapsed time.  This converts bursty input
	 * into steady output — OBS never sees a burst of audio. */
	if (s->active && s->first_audio) {
		int64_t wall_now = (int64_t)os_gettime_ns();
		int64_t frame_dur = s->audio_frame_dur_ns;
		if (frame_dur <= 0)
			frame_dur = 21333333LL; /* 1024 / 48000 */

		/* Pop up to 3 frames per tick to handle low-fps
		 * rendering (e.g. 30 fps → 33 ms between ticks,
		 * need ~1.5 audio frames per tick on average). */
		int pops = 0;
		while (pops < 3) {
			bool should_pop;
			if (s->last_audio_pop_time == 0) {
				should_pop = audio_buffer_is_ready(
					&s->audio_buf);
			} else {
				int64_t elapsed = wall_now -
					s->last_audio_pop_time;
				should_pop = (elapsed >= frame_dur);
			}
			if (!should_pop)
				break;

			struct audio_buf_frame *buf_frame;
			if (!audio_buffer_pop(&s->audio_buf, &buf_frame))
				break;

			/* Update frame duration estimate from actual
			 * popped frame */
			if (buf_frame->sample_rate > 0 &&
			    buf_frame->frames > 0)
				s->audio_frame_dur_ns =
					(int64_t)buf_frame->frames *
					1000000000LL /
					buf_frame->sample_rate;

			/* Advance pop timer.  First pop anchors to now;
			 * subsequent pops advance by frame_dur to stay
			 * in lockstep.  If we fall more than 2 frames
			 * behind (e.g. after a stall), re-anchor. */
			if (s->last_audio_pop_time == 0) {
				s->last_audio_pop_time = wall_now;
			} else {
				s->last_audio_pop_time += frame_dur;
				if (wall_now - s->last_audio_pop_time >
				    frame_dur * 2)
					s->last_audio_pop_time =
						wall_now - frame_dur;
			}

			/* ── Sample-rate adjustment ── */
			double rate = clock_tracker_get_smoothed_rate(
				&s->clock);

			uint32_t adjusted_sample_rate =
				buf_frame->sample_rate;
			bool in_warmup =
				(wall_now - s->stream_start_time) <
				SR_WARMUP_NS;
			bool outside_deadzone =
				fabs(rate - 1.0) > 0.04;

			if (outside_deadzone) {
				if (s->sr_hold_start == 0)
					s->sr_hold_start = wall_now;
			} else {
				s->sr_hold_start = 0;
			}

			bool held_long_enough =
				s->sr_hold_start != 0 &&
				(wall_now - s->sr_hold_start) >=
					SR_HOLD_TIME_NS;

			if (!in_warmup && held_long_enough) {
				uint32_t raw = (uint32_t)(
					(double)buf_frame->sample_rate *
					rate);
				adjusted_sample_rate =
					((raw + 50) / 100) * 100;
				if (adjusted_sample_rate <
				    buf_frame->sample_rate / 2)
					adjusted_sample_rate =
						buf_frame->sample_rate /
						2;
				if (adjusted_sample_rate >
				    buf_frame->sample_rate * 2)
					adjusted_sample_rate =
						buf_frame->sample_rate *
						2;
			}

			/* ── Timestamp computation ──
			 * sync_pts ON : 1 % drift correction + wall
			 *   clamp (tighter A/V lock).
			 * sync_pts OFF: 0.1 % drift correction + 20 ms
			 *   max-ahead clamp (independent). */
			int64_t out_ts;

			if (s->sync_pts &&
			    s->video_frames_out > 0) {
				if (s->audio_frames_out == 0) {
					out_ts = wall_now;
					s->audio_next_ts = wall_now;
					s->prev_audio_pts =
						buf_frame->pts_ns;
				} else {
					int64_t pts_delta =
						buf_frame->pts_ns -
						s->prev_audio_pts;
					s->prev_audio_pts =
						buf_frame->pts_ns;

					if (pts_delta > 0 &&
					    pts_delta < 500000000LL)
						s->audio_next_ts +=
							pts_delta;
					else
						s->audio_next_ts =
							wall_now;

					int64_t drift_error =
						wall_now -
						s->audio_next_ts;
					if (drift_error < 0)
						s->audio_next_ts +=
							drift_error / 10;
					else if (drift_error >
						 100000000LL)
						s->audio_next_ts +=
							drift_error / 100;
					else
						s->audio_next_ts +=
							drift_error / 100;

					if (s->audio_next_ts > wall_now)
						s->audio_next_ts =
							wall_now;

					/* Hard behind-clamp: if we fell
					 * >200ms behind (e.g. after a
					 * network stall), snap to now.
					 * Without this, 1% drift takes
					 * ~10s to recover and OBS
					 * accumulates audio buffering
					 * for every behind-ts frame. */
					if (s->audio_next_ts <
					    wall_now - 200000000LL)
						s->audio_next_ts =
							wall_now;

					out_ts = s->audio_next_ts;
				}
			} else if (s->audio_frames_out == 0) {
				out_ts = wall_now;
				s->audio_next_ts = wall_now;
				s->prev_audio_pts =
					buf_frame->pts_ns;
			} else {
				int64_t pts_delta =
					buf_frame->pts_ns -
					s->prev_audio_pts;
				s->prev_audio_pts =
					buf_frame->pts_ns;

				if (pts_delta > 0 &&
				    pts_delta < 500000000LL)
					s->audio_next_ts += pts_delta;
				else
					s->audio_next_ts = wall_now;

				int64_t drift_error =
					wall_now - s->audio_next_ts;
				if (drift_error < 0)
					s->audio_next_ts +=
						drift_error / 10;
				else if (drift_error > 100000000LL)
					s->audio_next_ts +=
						drift_error / 100;
				else
					s->audio_next_ts +=
						drift_error / 1000;

				int64_t max_ahead =
					wall_now + 20000000LL;
				if (s->audio_next_ts > max_ahead)
					s->audio_next_ts = max_ahead;

				/* Hard behind-clamp (same as
				 * sync_pts path above) */
				if (s->audio_next_ts <
				    wall_now - 200000000LL)
					s->audio_next_ts = wall_now;

				out_ts = s->audio_next_ts;
			}
			s->audio_out_ts = out_ts;

			/* ── Output to OBS ── */
			struct obs_source_audio obs_audio = {0};
			for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++)
				obs_audio.data[i] = buf_frame->data[i];

			obs_audio.frames = buf_frame->frames;
			obs_audio.samples_per_sec =
				adjusted_sample_rate;
			obs_audio.format =
				(enum audio_format)buf_frame->format;
			obs_audio.speakers =
				(enum speaker_layout)
					buf_frame->speakers;
			obs_audio.timestamp = out_ts;

			obs_source_output_audio(s->source, &obs_audio);
			s->audio_frames_out++;
			pops++;

			/* ── Diagnostic logging every ~5 s ── */
			if ((wall_now - s->last_diag_time) >=
			    5000000000LL) {
				int64_t av_wall = 0;
				if (s->video_frames_out > 0)
					av_wall = s->audio_out_ts -
						  s->video_out_ts;
				SM_LOG(LOG_INFO,
				       "DIAG: rate=%.4f adj_sr=%u "
				       "buf=%lldms av_wall=%lldms "
				       "a_out=%llu v_out=%llu%s",
				       rate, adjusted_sample_rate,
				       (long long)(audio_buffer_level_ns(
							&s->audio_buf) /
						  1000000),
				       (long long)(av_wall / 1000000),
				       (unsigned long long)
					       s->audio_frames_out,
				       (unsigned long long)
					       s->video_frames_out,
				       s->sync_pts ? " [PTS-SYNC]"
						   : "");
				s->last_diag_time = wall_now;
			}
		}
	}

	/* Check if media ended and needs reconnect */
	bool should_be_live = s->close_when_inactive
				      ? obs_source_showing(s->source)
				      : obs_source_active(s->source);
	if (!s->active && !s->reconnecting && s->url && *s->url &&
	    should_be_live) {
		pthread_mutex_lock(&s->state_mutex);
		bool ended = (s->state == OBS_MEDIA_STATE_ENDED);
		pthread_mutex_unlock(&s->state_mutex);

		if (ended)
			schedule_reconnect(s);
	}
}

/* Media control callbacks */
static void smooth_media_restart(void *data)
{
	struct smooth_media_source *s = data;
	if (obs_source_showing(s->source))
		start_media(s);
}

static void smooth_media_stop(void *data)
{
	struct smooth_media_source *s = data;
	stop_media_thread(s);
	obs_source_output_video(s->source, NULL);

	pthread_mutex_lock(&s->state_mutex);
	s->state = OBS_MEDIA_STATE_STOPPED;
	pthread_mutex_unlock(&s->state_mutex);
}

static enum obs_media_state smooth_media_get_state(void *data)
{
	struct smooth_media_source *s = data;
	return s->state;
}

/* ──────────────────────────────────────────────
 *  OBS source_info registration struct
 * ────────────────────────────────────────────── */

struct obs_source_info smooth_media_source_info = {
	.id             = "smooth_media_source",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			  OBS_SOURCE_DO_NOT_DUPLICATE |
			  OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name       = smooth_media_get_name,
	.create         = smooth_media_create,
	.destroy        = smooth_media_destroy,
	.get_defaults   = smooth_media_defaults,
	.get_properties = smooth_media_get_properties,
	.activate       = smooth_media_activate,
	.deactivate     = smooth_media_deactivate,
	.show           = smooth_media_show,
	.hide           = smooth_media_hide,
	.video_tick     = smooth_media_tick,
	.update         = smooth_media_update,
	.icon_type      = OBS_ICON_TYPE_MEDIA,
	.media_restart  = smooth_media_restart,
	.media_stop     = smooth_media_stop,
	.media_get_state = smooth_media_get_state,
};
