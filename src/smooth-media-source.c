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
#define SR_WARMUP_NS        (8000000000LL) /* 8s: hold rate at 1.0x until the post-connect burst ages out of the clock window */
#define SR_SLEW_PER_POP     0.0005         /* max declared-rate change per popped frame (~2.3%/s) — no audible pitch steps */
#define SR_SLOW_ALPHA       0.0015         /* per-tick EMA on the drift estimate (~11s TC) — rejects jitter-induced rate wobble so the declared rate stays put */
#define SR_UPDATE_DEADBAND_HZ 40.0         /* only re-declare sample rate after it drifts this far — keeps OBS's resampler from resetting (which clicks) */
#define SR_MIN_HOLD_NS      (2000000000LL) /* and never re-declare more often than this */
#define CLOCK_SETTLE_NS     (2000000000LL) /* ignore the first 2s of audio for rate measurement: SRT flushes its buffer in a burst at connect, which would otherwise poison the rate estimate */
#define STALL_GAP_NS        (500000000LL)  /* an audio arrival gap larger than this is treated as a stall; rate measurement re-settles afterward so the stall+catch-up burst can't corrupt it. Kept well above normal jitter (which can momentarily exceed 250ms) so it only trips on genuine freezes. */
#define SR_STABLE_JITTER_NS (50000000LL)   /* only change the declared sample rate while jitter is below this (link calm) — prevents chasing the rate during a disruption */

/* Forward declarations */
static void smooth_media_update(void *data, obs_data_t *settings);

/* ──────────────────────────────────────────────
 *  Format conversion helpers (AVFormat → OBS)
 * ────────────────────────────────────────────── */

static inline enum video_format av_to_obs_video_format(int f)
{
	switch (f) {
	case AV_PIX_FMT_YUV420P:   return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUVJ420P:  return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUYV422:   return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_YUV422P:   return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUVJ422P:  return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P:   return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_YUVJ444P:  return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_UYVY422:   return VIDEO_FORMAT_UYVY;
	case AV_PIX_FMT_YVYU422:   return VIDEO_FORMAT_YVYU;
	case AV_PIX_FMT_NV12:      return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_RGBA:      return VIDEO_FORMAT_RGBA;
	case AV_PIX_FMT_BGRA:      return VIDEO_FORMAT_BGRA;
	case AV_PIX_FMT_YUVA420P:  return VIDEO_FORMAT_I40A;
	case AV_PIX_FMT_YUV420P10LE: return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_YUV422P10LE: return VIDEO_FORMAT_I210;
	case AV_PIX_FMT_YUV444P10LE: return VIDEO_FORMAT_I412;
	case AV_PIX_FMT_YUVA422P:  return VIDEO_FORMAT_I42A;
	case AV_PIX_FMT_YUVA444P:  return VIDEO_FORMAT_YUVA;
	case AV_PIX_FMT_BGR0:      return VIDEO_FORMAT_BGRX;
	case AV_PIX_FMT_P010LE:    return VIDEO_FORMAT_P010;
	case AV_PIX_FMT_GRAY8:     return VIDEO_FORMAT_Y800;
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

	/* Audio-only mode: keep decoding (so video resumes instantly when
	 * re-enabled, with valid reference frames) but don't push video to
	 * OBS. Lets you test audio over a slow remote-desktop session. */
	if (s->disable_video)
		return;

	/* NOTE: we intentionally do NOT feed video PTS into the clock
	 * tracker.  Video decoding is susceptible to GPU-contention
	 * bursts (e.g. when multiple NVENC sessions are running),
	 * which cause many frames to be decoded in quick succession.
	 * This makes stream_elapsed >> wall_elapsed and poisons the
	 * rate measurement.  Audio from the network is a far more
	 * reliable clock source. */
	int64_t wall_now = (int64_t)os_gettime_ns();

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
	 * Video is offset forward by the (adaptive) jitter buffer depth so
	 * OBS holds the frame before displaying it. This compensates for the
	 * delay audio experiences sitting in the jitter buffer and keeps lips
	 * synced as the target depth adapts to link jitter. */
	int64_t buf_offset = audio_buffer_target_ns(&s->audio_buf);
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
	bool is_yuvj = (vf->format == AV_PIX_FMT_YUVJ420P ||
			vf->format == AV_PIX_FMT_YUVJ422P ||
			vf->format == AV_PIX_FMT_YUVJ444P);
	enum video_range_type range =
		(vf->color_range == AVCOL_RANGE_JPEG || is_yuvj)
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

	/* Feed the rate estimator only with steady-state delivery. Two cases
	 * are excluded because they make the estimate read garbage and the
	 * sample-rate corrector then chases it (pitch wobble + resampler-reset
	 * clicks):
	 *   1. Connect: the SRT server dumps its backlog in a fast burst.
	 *   2. Stalls: while the stream is frozen, wall time advances but PTS
	 *      doesn't, so the rate dives toward 0.90; then the catch-up burst
	 *      swings it back. A single blip otherwise causes ~40s of wobble.
	 * Detecting an arrival gap re-arms the settle window so the stall AND
	 * its catch-up burst are skipped; the rate simply holds its last-good
	 * value through the disruption. */
	int64_t wall_now = (int64_t)os_gettime_ns();
	if (s->last_audio_arrival_ns != 0 &&
	    wall_now - s->last_audio_arrival_ns > STALL_GAP_NS)
		s->clock_skip_until_ns = wall_now + CLOCK_SETTLE_NS;
	s->last_audio_arrival_ns = wall_now;

	if (wall_now >= s->clock_skip_until_ns)
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
			  af->pts_ns, wall_now);

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
	s->underrun_count = 0;
	s->sr_change_count = 0;
	s->last_underrun_log_time = 0;
	s->stream_start_time = (int64_t)os_gettime_ns();
	s->clock_skip_until_ns = s->stream_start_time + CLOCK_SETTLE_NS;
	s->last_audio_arrival_ns = 0;
	s->sr_ratio = 1.0;
	s->sr_slow_rate = 1.0;
	s->declared_sr = 0;
	s->last_sr_change_ns = 0;
	s->last_audio_pop_time = 0;
	s->audio_frame_dur_ns = 0;
	s->prev_video_pts = 0;
	s->video_next_ts = 0;
	s->prev_audio_pts = 0;
	s->audio_next_ts = 0;
	s->active = true;
	s->kill = false;

	/* Configure jitter buffer floor; the target/ceiling self-tune from
	 * here based on measured link jitter and underruns. */
	s->audio_buf.min_buffer_ns = JITTER_BUFFER_MS * 1000000LL;
	s->audio_buf.target_buffer_ns = JITTER_BUFFER_MS * 1000000LL;
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

	/* Clear reconnecting flag so smooth_media_tick can schedule another
	 * reconnect if this attempt also fails.
	 *
	 * IMPORTANT: this is a plain (volatile) write with NO mutex. Other
	 * threads (hide/deactivate/update/destroy) call pthread_join on this
	 * thread *while holding* reconnect_mutex; if we tried to take that
	 * mutex here we would deadlock against the joiner. 'reconnecting' is
	 * only an advisory flag, so a lock-free write is safe. */
	s->reconnecting = false;

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
	obs_data_set_default_int(settings, "reconnect_delay_sec", 5);
	obs_data_set_default_bool(settings, "hw_decode", false);
	obs_data_set_default_bool(settings, "sync_pts", false);
	obs_data_set_default_bool(settings, "close_when_inactive", true);
	obs_data_set_default_bool(settings, "disable_video", false);
	obs_data_set_default_bool(settings, "debug_logging", false);
}

static obs_properties_t *smooth_media_get_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	/* ── Branded header ── */
	obs_property_t *hdr = obs_properties_add_text(props, "plugin_info",
		"<a href='https://IRLhosting.com'>"
		"IRLhosting Smooth Player</a>"
		" &nbsp;v" SMOOTH_MEDIA_VERSION,
		OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(hdr, true);

	/* ── Connection ── */
	obs_properties_t *conn = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_text(conn, "input",
				    "Stream URL", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(p,
		"Full stream URL including protocol.\n"
		"Examples:\n"
		"  srt://host:port?streamid=...\n"
		"  rtmp://host/app/key\n"
		"  rist://host:port");

	p = obs_properties_add_text(conn, "input_format",
				    "Input Format", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(p,
		"Force a specific container format (e.g. mpegts).\n"
		"Leave blank for auto-detection.");

	p = obs_properties_add_int_slider(conn, "reconnect_delay_sec",
					  "Reconnect Delay", 1, 60, 1);
	obs_property_int_set_suffix(p, " s");
	obs_property_set_long_description(p,
		"Seconds between reconnection attempts when the stream drops.");

	obs_properties_add_group(props, "grp_connection",
				 "Connection", OBS_GROUP_NORMAL, conn);

	/* ── Playback ── */
	obs_properties_t *play = obs_properties_create();

	p = obs_properties_add_bool(play, "hw_decode",
				    "Hardware Decoding");
	obs_property_set_long_description(p,
		"Use GPU-accelerated decoding (NVDEC, QSV, VAAPI).\n"
		"Reduces CPU load but may not support all codecs.");

	p = obs_properties_add_bool(play, "sync_pts",
				    "Sync A/V via PTS");
	obs_property_set_long_description(p,
		"Use presentation timestamps for audio-video sync.\n"
		"May help streams with inconsistent frame timing.");

	p = obs_properties_add_bool(play, "close_when_inactive",
				    "Close When Inactive");
	obs_property_set_long_description(p,
		"Stop playback and disconnect when the source is\n"
		"hidden or on another scene. Saves CPU, bandwidth,\n"
		"and server resources.");

	p = obs_properties_add_bool(play, "disable_video",
				    "Disable Video Preview (Audio Only)");
	obs_property_set_long_description(p,
		"Stop sending video to OBS while keeping the stream\n"
		"connected and audio playing. Useful when working over\n"
		"a remote desktop session, where rendering the preview\n"
		"is slow — toggle this on to test audio without the\n"
		"video preview. Applies instantly without reconnecting.");

	obs_properties_add_group(props, "grp_playback",
				 "Playback", OBS_GROUP_NORMAL, play);

	/* ── Advanced ── */
	obs_properties_t *adv = obs_properties_create();

	p = obs_properties_add_text(adv, "ffmpeg_options",
				    "FFmpeg Options", OBS_TEXT_DEFAULT);
	obs_property_set_long_description(p,
		"Extra FFmpeg demuxer/decoder options.\n"
		"Example: analyzeduration=2000000 probesize=5000000");

	p = obs_properties_add_bool(adv, "debug_logging",
				    "Verbose Debug Logging");
	obs_property_set_long_description(p,
		"Log detailed timing to the OBS log every second, plus\n"
		"events (underruns, dropped frames, sample-rate changes).\n"
		"Use this when diagnosing clicks/stutter, then turn it\n"
		"back off — it's noisy. Applies instantly.");

	obs_properties_add_group(props, "grp_advanced",
				 "Advanced", OBS_GROUP_NORMAL, adv);

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
	clock_tracker_free(&s->clock);

	pthread_mutex_destroy(&s->reconnect_mutex);
	pthread_mutex_destroy(&s->state_mutex);
	os_event_destroy(s->reconnect_stop_event);

	bfree(s->url);
	bfree(s->input_format);
	bfree(s->ffmpeg_options);
	bfree(s);
}

/* Compare two possibly-NULL strings for inequality. */
static bool str_changed(const char *a, const char *b)
{
	if (!a && !b)
		return false;
	if (!a || !b)
		return true;
	return strcmp(a, b) != 0;
}

static void smooth_media_update(void *data, obs_data_t *settings)
{
	struct smooth_media_source *s = data;

	stop_reconnect_thread(s);

	const char *url = obs_data_get_string(settings, "input");
	const char *fmt = obs_data_get_string(settings, "input_format");
	const char *opts = obs_data_get_string(settings, "ffmpeg_options");

	char *new_url = (url && *url) ? bstrdup(url) : NULL;
	char *new_fmt = (fmt && *fmt) ? bstrdup(fmt) : NULL;
	char *new_opts = (opts && *opts) ? bstrdup(opts) : NULL;
	bool new_hw = obs_data_get_bool(settings, "hw_decode");

	/* Only the settings that affect the decoder pipeline force a
	 * reconnect. Playback-only toggles (sync_pts, disable_video,
	 * close_when_inactive, reconnect delay) apply live so flipping
	 * them never interrupts the stream. */
	bool stream_changed = str_changed(s->url, new_url) ||
			      str_changed(s->input_format, new_fmt) ||
			      str_changed(s->ffmpeg_options, new_opts) ||
			      s->hw_decode != new_hw;

	bfree(s->url);
	bfree(s->input_format);
	bfree(s->ffmpeg_options);
	s->url = new_url;
	s->input_format = new_fmt;
	s->ffmpeg_options = new_opts;
	s->hw_decode = new_hw;

	s->sync_pts = obs_data_get_bool(settings, "sync_pts");
	s->close_when_inactive = obs_data_get_bool(settings, "close_when_inactive");
	s->disable_video = obs_data_get_bool(settings, "disable_video");
	s->debug_logging = obs_data_get_bool(settings, "debug_logging");
	s->reconnect_delay_sec = (int)obs_data_get_int(settings, "reconnect_delay_sec");

	if (s->reconnect_delay_sec < 1)
		s->reconnect_delay_sec = 10;

	bool should_run = s->url && *s->url &&
			  (!s->close_when_inactive ||
			   obs_source_showing(s->source));

	if (!s->url || !*s->url) {
		stop_media_thread(s);
	} else if (stream_changed) {
		if (should_run)
			start_media(s);
		else
			stop_media_thread(s);
	} else if (should_run && !s->active && !s->media_thread_valid) {
		start_media(s);
	} else if (!should_run && s->active) {
		stop_media_thread(s);
	}

	/* Blank the preview immediately when switching to audio-only. */
	if (s->disable_video)
		obs_source_output_video(s->source, NULL);
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

		/* Measured stream delivery rate (clamped to sane drift). */
		double rate = clock_tracker_get_smoothed_rate(&s->clock);
		if (rate < 0.90)
			rate = 0.90;
		if (rate > 1.10)
			rate = 1.10;

		/* ── Closed-loop drain pacing ──
		 * Pop cadence is paced to the stream's TRUE delivery rate, so
		 * the buffer neither drains nor fills under sustained drift —
		 * the open-loop fixed-1.0x cadence used to empty the buffer in
		 * ~4s on a 0.98x stream, killing all jitter protection. A mild
		 * proportional term nudges the level back toward the adaptive
		 * target after disturbances. */
		int64_t target = audio_buffer_target_ns(&s->audio_buf);
		int64_t level = audio_buffer_level_ns(&s->audio_buf);
		double lvl_err = 0.0;
		if (target > 0) {
			lvl_err = (double)(level - target) / (double)target;
			/* Refill-only: when below target, slow the drain to let
			 * it rebuild; when above, do NOT speed up (that over-
			 * drains and can push extra audio into OBS) — the
			 * drop-oldest ceiling bounds excess latency instead. */
			if (lvl_err > 0.0)
				lvl_err = 0.0;
			if (lvl_err < -0.5)
				lvl_err = -0.5;
		}
		int64_t pop_interval =
			(int64_t)((double)frame_dur / rate *
				  (1.0 - 0.15 * lvl_err));

		/* ── Continuous, slew-limited sample-rate match ──
		 * Declare the stream's actual rate so OBS consumes at the
		 * delivery pace. No deadzone (small drift is corrected too) and
		 * no round-to-100 (which caused ~2kHz/4% audible pitch steps);
		 * the per-pop slew limit keeps changes inaudible. */
		/* Heavily smooth the drift estimate so jitter-induced wobble in
		 * the measured rate doesn't keep nudging the declared rate
		 * (which would reset OBS's resampler and click). Real clock
		 * drift is slow and constant, so a long time-constant is fine. */
		s->sr_slow_rate += SR_SLOW_ALPHA * (rate - s->sr_slow_rate);
		bool in_warmup =
			(wall_now - s->stream_start_time) < SR_WARMUP_NS;
		double desired_ratio = in_warmup ? 1.0 : s->sr_slow_rate;
		if (desired_ratio < 0.95)
			desired_ratio = 0.95;
		if (desired_ratio > 1.05)
			desired_ratio = 1.05;
		if (s->sr_ratio <= 0.0)
			s->sr_ratio = 1.0;

		int pops = 0;
		/* Cap at 8 so a post-stall backlog can be drained over a tick
		 * without an unbounded burst. */
		while (pops < 8) {
			bool should_pop;
			if (s->last_audio_pop_time == 0) {
				should_pop = audio_buffer_is_ready(
					&s->audio_buf);
			} else {
				int64_t elapsed = wall_now -
					s->last_audio_pop_time;
				should_pop = (elapsed >= pop_interval);
			}
			if (!should_pop)
				break;

			struct audio_buf_frame *buf_frame;
			if (!audio_buffer_pop(&s->audio_buf, &buf_frame)) {
				/* Wanted audio but buffer empty → underrun;
				 * grow the adaptive cushion so we rebuild a
				 * deeper buffer after the stall. */
				if (s->audio_frames_out > 0) {
					audio_buffer_note_underrun(
						&s->audio_buf);
					s->underrun_count++;
					if (s->debug_logging &&
					    (wall_now - s->last_underrun_log_time)
						    >= 500000000LL) {
						SM_LOG(LOG_INFO,
						       "DBG underrun #%llu (buf empty, tgt now %lldms)",
						       (unsigned long long)
							       s->underrun_count,
						       (long long)(audio_buffer_target_ns(
									&s->audio_buf) /
								1000000));
						s->last_underrun_log_time =
							wall_now;
					}
				}
				break;
			}

			/* Update frame duration estimate from actual
			 * popped frame */
			if (buf_frame->sample_rate > 0 &&
			    buf_frame->frames > 0)
				s->audio_frame_dur_ns =
					(int64_t)buf_frame->frames *
					1000000000LL /
					buf_frame->sample_rate;

			/* Advance pop timer.  First pop anchors to now;
			 * subsequent pops advance by pop_interval to stay
			 * in lockstep.  If we fall more than 2 intervals
			 * behind (e.g. after a stall), re-anchor. */
			if (s->last_audio_pop_time == 0) {
				s->last_audio_pop_time = wall_now;
			} else {
				s->last_audio_pop_time += pop_interval;
				if (wall_now - s->last_audio_pop_time >
				    pop_interval * 2)
					s->last_audio_pop_time =
						wall_now - pop_interval;
			}

			/* Slew the declared-rate ratio toward target. */
			double dr = desired_ratio - s->sr_ratio;
			if (dr > SR_SLEW_PER_POP)
				dr = SR_SLEW_PER_POP;
			if (dr < -SR_SLEW_PER_POP)
				dr = -SR_SLEW_PER_POP;
			s->sr_ratio += dr;

			/* Declared sample rate is held STABLE via a deadband:
			 * OBS rebuilds its resampler whenever samples_per_sec
			 * changes, which clicks. So we only re-declare once the
			 * ideal rate has drifted >=25Hz from what OBS currently
			 * has — in steady state it never changes (zero clicks),
			 * and any change is an inaudible <0.06% step. */
			double exact = (double)buf_frame->sample_rate *
				       s->sr_ratio;
			if (s->declared_sr == 0) {
				s->declared_sr = (uint32_t)(exact + 0.5);
				s->last_sr_change_ns = wall_now;
			} else if (fabs(exact - (double)s->declared_sr) >=
					   SR_UPDATE_DEADBAND_HZ &&
				   (wall_now - s->last_sr_change_ns) >=
					   SR_MIN_HOLD_NS &&
				   audio_buffer_jitter_ns(&s->audio_buf) <
					   SR_STABLE_JITTER_NS) {
				uint32_t old_sr = s->declared_sr;
				s->declared_sr = (uint32_t)(exact + 0.5);
				s->last_sr_change_ns = wall_now;
				s->sr_change_count++;
				if (s->debug_logging)
					SM_LOG(LOG_INFO,
					       "DBG sample-rate change #%llu: %u -> %u Hz (slow_rate=%.5f) [resampler reset]",
					       (unsigned long long)
						       s->sr_change_count,
					       old_sr, s->declared_sr,
					       s->sr_slow_rate);
			}
			uint32_t adjusted_sample_rate = s->declared_sr;

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

					/* sync_pts: pull toward wall clock —
					 * 10% when ahead (snap back), a
					 * constant 1% when behind for a
					 * tight A/V lock. */
					int64_t drift_error =
						wall_now -
						s->audio_next_ts;
					if (drift_error < 0)
						s->audio_next_ts +=
							drift_error / 10;
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

			/* One-time confirmation of exactly what we hand OBS, so
			 * "no audio" reports can be triaged: if these look sane
			 * and the mixer meter still doesn't move, the issue is
			 * OBS-side routing (mute / track / monitoring). */
			if (s->audio_frames_out == 0)
				SM_LOG(LOG_INFO,
				       "First audio -> OBS: fmt=%d speakers=%d sr=%u frames=%u ts=%lldms",
				       (int)obs_audio.format,
				       (int)obs_audio.speakers,
				       obs_audio.samples_per_sec,
				       obs_audio.frames,
				       (long long)(out_ts / 1000000));

			obs_source_output_audio(s->source, &obs_audio);
			s->audio_frames_out++;
			pops++;

			/* ── Diagnostic logging ──
			 * Normal: a compact line every 5 s.
			 * Debug:  a detailed line every 1 s. */
			int64_t diag_interval = s->debug_logging
							? 1000000000LL
							: 5000000000LL;
			if ((wall_now - s->last_diag_time) >= diag_interval) {
				int64_t av_wall = 0;
				if (s->video_frames_out > 0)
					av_wall = s->audio_out_ts -
						  s->video_out_ts;
				if (s->debug_logging) {
					SM_LOG(LOG_INFO,
					       "DBG rate=%.4f slow=%.4f sr_ratio=%.5f decl_sr=%u "
					       "buf=%lldms tgt=%lldms jit=%lldms pop_iv=%lldms "
					       "av=%lldms a=%llu v=%llu drop=%llu under=%llu sr_chg=%llu%s",
					       rate, s->sr_slow_rate,
					       s->sr_ratio, adjusted_sample_rate,
					       (long long)(audio_buffer_level_ns(
							&s->audio_buf) / 1000000),
					       (long long)(audio_buffer_target_ns(
							&s->audio_buf) / 1000000),
					       (long long)(audio_buffer_jitter_ns(
							&s->audio_buf) / 1000000),
					       (long long)(pop_interval / 1000000),
					       (long long)(av_wall / 1000000),
					       (unsigned long long)s->audio_frames_out,
					       (unsigned long long)s->video_frames_out,
					       (unsigned long long)s->audio_buf.frames_dropped,
					       (unsigned long long)s->underrun_count,
					       (unsigned long long)s->sr_change_count,
					       s->sync_pts ? " [PTS-SYNC]" : "");
				} else {
					SM_LOG(LOG_INFO,
					       "DIAG: rate=%.4f adj_sr=%u "
					       "buf=%lldms tgt=%lldms av_wall=%lldms "
					       "a_out=%llu v_out=%llu%s",
					       rate, adjusted_sample_rate,
					       (long long)(audio_buffer_level_ns(
							&s->audio_buf) / 1000000),
					       (long long)(audio_buffer_target_ns(
							&s->audio_buf) / 1000000),
					       (long long)(av_wall / 1000000),
					       (unsigned long long)s->audio_frames_out,
					       (unsigned long long)s->video_frames_out,
					       s->sync_pts ? " [PTS-SYNC]" : "");
				}
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
