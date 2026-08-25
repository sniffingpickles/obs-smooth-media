#include "smooth-media-source.h"

#include <libavutil/pixdesc.h>
#include <limits.h>
#include <stdio.h>
#include <util/dstr.h>
#include <util/platform.h>

#define PLUGIN_LOG_PREFIX "[Smooth Media Source '%s']: "
#define SM_LOG(level, format, ...) \
	blog(level, PLUGIN_LOG_PREFIX format, \
	     obs_source_get_name(s->source), ##__VA_ARGS__)

/* Controller constants validated by the timing and network test suites. */
#define NETWORK_BUFFER_MB           2
#define JITTER_BUFFER_MS            80
#define SR_WARMUP_NS                (8000000000LL)
#define SR_SLEW_PER_POP             0.0005
#define SR_EMERGENCY_SLEW_PER_POP   0.0015
#define SR_SLOW_ALPHA               0.0015
#define PLAYBACK_RATIO_MIN          0.90
#define PLAYBACK_RATIO_MAX          1.10
#define CLOCK_SETTLE_NS             SR_WARMUP_NS
/* MPEG-TS audio can arrive in ordinary ~600ms batches. */
#define STALL_GAP_NS                (2000000000LL)

/* Forward declarations */
static void smooth_media_update(void *data, obs_data_t *settings);
static void start_media_locked(struct smooth_media_source *s);

static bool source_is_active(const struct smooth_media_source *s)
{
	return os_atomic_load_bool(&s->active);
}

static bool source_is_killed(const struct smooth_media_source *s)
{
	return os_atomic_load_bool(&s->kill);
}

static bool debug_logging_enabled(const struct smooth_media_source *s)
{
	return os_atomic_load_bool(&s->debug_logging);
}

static void set_media_state(struct smooth_media_source *s,
			    enum obs_media_state state)
{
	pthread_mutex_lock(&s->state_mutex);
	s->state = state;
	pthread_mutex_unlock(&s->state_mutex);
}

static enum obs_media_state get_media_state(struct smooth_media_source *s)
{
	pthread_mutex_lock(&s->state_mutex);
	enum obs_media_state state = s->state;
	pthread_mutex_unlock(&s->state_mutex);
	return state;
}

/* Format conversion helpers */

static inline enum video_format av_to_obs_video_format(int f)
{
	switch (f) {
	case AV_PIX_FMT_YUV420P: return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUVJ420P: return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUYV422: return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_YUV422P: return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUVJ422P: return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV444P: return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_YUVJ444P: return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_UYVY422: return VIDEO_FORMAT_UYVY;
	case AV_PIX_FMT_YVYU422: return VIDEO_FORMAT_YVYU;
	case AV_PIX_FMT_NV12: return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_RGBA: return VIDEO_FORMAT_RGBA;
	case AV_PIX_FMT_BGRA: return VIDEO_FORMAT_BGRA;
	case AV_PIX_FMT_YUVA420P: return VIDEO_FORMAT_I40A;
	case AV_PIX_FMT_YUV420P10LE: return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_YUV422P10LE: return VIDEO_FORMAT_I210;
	case AV_PIX_FMT_YUV444P10LE: return VIDEO_FORMAT_I412;
	case AV_PIX_FMT_YUVA422P: return VIDEO_FORMAT_I42A;
	case AV_PIX_FMT_YUVA444P: return VIDEO_FORMAT_YUVA;
	case AV_PIX_FMT_BGR0: return VIDEO_FORMAT_BGRX;
	case AV_PIX_FMT_P010LE: return VIDEO_FORMAT_P010;
	case AV_PIX_FMT_GRAY8: return VIDEO_FORMAT_Y800;
	default: return VIDEO_FORMAT_NONE;
	}
}

static inline enum audio_format av_to_obs_audio_format(int f)
{
	switch (f) {
	case AV_SAMPLE_FMT_U8: return AUDIO_FORMAT_U8BIT;
	case AV_SAMPLE_FMT_S16: return AUDIO_FORMAT_16BIT;
	case AV_SAMPLE_FMT_S32: return AUDIO_FORMAT_32BIT;
	case AV_SAMPLE_FMT_FLT: return AUDIO_FORMAT_FLOAT;
	case AV_SAMPLE_FMT_U8P: return AUDIO_FORMAT_U8BIT_PLANAR;
	case AV_SAMPLE_FMT_S16P: return AUDIO_FORMAT_16BIT_PLANAR;
	case AV_SAMPLE_FMT_S32P: return AUDIO_FORMAT_32BIT_PLANAR;
	case AV_SAMPLE_FMT_FLTP: return AUDIO_FORMAT_FLOAT_PLANAR;
	default: return AUDIO_FORMAT_UNKNOWN;
	}
}

static inline enum AVSampleFormat obs_to_av_audio_format(int f)
{
	switch (f) {
	case AUDIO_FORMAT_U8BIT: return AV_SAMPLE_FMT_U8;
	case AUDIO_FORMAT_16BIT: return AV_SAMPLE_FMT_S16;
	case AUDIO_FORMAT_32BIT: return AV_SAMPLE_FMT_S32;
	case AUDIO_FORMAT_FLOAT: return AV_SAMPLE_FMT_FLT;
	case AUDIO_FORMAT_U8BIT_PLANAR: return AV_SAMPLE_FMT_U8P;
	case AUDIO_FORMAT_16BIT_PLANAR: return AV_SAMPLE_FMT_S16P;
	case AUDIO_FORMAT_32BIT_PLANAR: return AV_SAMPLE_FMT_S32P;
	case AUDIO_FORMAT_FLOAT_PLANAR: return AV_SAMPLE_FMT_FLTP;
	default: return AV_SAMPLE_FMT_NONE;
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

static int64_t saturating_add_i64(int64_t a, int64_t b)
{
	if (b > 0 && a > INT64_MAX - b)
		return INT64_MAX;
	if (b < 0 && a < INT64_MIN - b)
		return INT64_MIN;
	return a + b;
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
	case AVCOL_SPC_SMPTE240M: return VIDEO_CS_601;
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

/* Decoder callbacks */

static void on_video_frame(void *opaque, struct decoded_video_frame *vf)
{
	struct smooth_media_source *s = opaque;

	int plane_count =
		av_pix_fmt_count_planes((enum AVPixelFormat)vf->format);
	if (vf->width <= 0 || vf->height <= 0 ||
	    av_image_check_size((unsigned int)vf->width,
				(unsigned int)vf->height, 0, NULL) < 0 ||
	    plane_count <= 0 || plane_count > 4)
		return;
	for (int i = 0; i < plane_count; i++) {
		int min_stride = av_image_get_linesize(
			(enum AVPixelFormat)vf->format, vf->width, i);
		if (!vf->data[i] || min_stride < 0 ||
		    vf->linesize[i] < min_stride)
			return;
	}

	enum video_format obs_fmt = av_to_obs_video_format(vf->format);
	if (obs_fmt == VIDEO_FORMAT_NONE)
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
	if (os_atomic_load_bool(&s->disable_video))
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
		SM_LOG(LOG_INFO, "First video frame: %dx%d fmt=%d pts=%lldms",
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
	struct audio_buffer_stats buffer_stats;
	audio_buffer_get_stats(&s->audio_buf, &buffer_stats);
	/* Match video delay to the audio actually queued, not only the desired
	 * target. During a fast feed or recovery refill, level can sit above
	 * target; using target alone makes video lead the buffered audio. */
	int64_t buf_offset = buffer_stats.level_ns;
	if (buf_offset < buffer_stats.target_ns)
		buf_offset = buffer_stats.target_ns;
	if (buf_offset > buffer_stats.max_ns)
		buf_offset = buffer_stats.max_ns;
	int64_t out_ts;

	pthread_mutex_lock(&s->timing_mutex);
	bool first_output = s->video_frames_out == 0;
	pthread_mutex_unlock(&s->timing_mutex);

	if (first_output) {
		out_ts = wall_now + buf_offset;
		s->video_next_ts = wall_now + buf_offset;
		s->prev_video_pts = vf->pts_ns;
	} else {
		int64_t prev_video_pts = s->prev_video_pts;
		uint64_t pts_delta_u =
			(uint64_t)vf->pts_ns - (uint64_t)prev_video_pts;
		s->prev_video_pts = vf->pts_ns;

		if (vf->pts_ns > prev_video_pts &&
		    pts_delta_u < UINT64_C(500000000)) {
			int64_t pts_delta = (int64_t)pts_delta_u;
			/* Convert media-clock spacing to wall-clock spacing. A
			 * 0.98x stream's 16.67 ms PTS step is about 17.0 ms in
			 * realtime; using the raw delta made video drift away
			 * from audio by tens of milliseconds per minute. */
			/* Use the same playback-rate ratio currently applied to
			 * audio. Following the faster raw clock estimate here while
			 * audio deliberately slews its speed caused a
			 * transition-period lip-sync error. */
			pthread_mutex_lock(&s->controller_mutex);
			double rate = s->playback_ratio;
			pthread_mutex_unlock(&s->controller_mutex);
			if (rate != rate)
				rate = 1.0;
			else if (rate < PLAYBACK_RATIO_MIN)
				rate = PLAYBACK_RATIO_MIN;
			else if (rate > PLAYBACK_RATIO_MAX)
				rate = PLAYBACK_RATIO_MAX;
			s->video_next_ts = saturating_add_i64(
				s->video_next_ts,
				(int64_t)((double)pts_delta / rate));
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
	pthread_mutex_lock(&s->timing_mutex);
	if (s->video_frames_out > 0 && out_ts <= s->video_out_ts)
		out_ts = s->video_out_ts + 1;
	s->video_next_ts = out_ts;
	s->video_out_ts = out_ts;
	s->video_frames_out++;
	pthread_mutex_unlock(&s->timing_mutex);

	/* Build OBS frame */
	struct obs_source_frame frame = {0};
	enum video_colorspace space = av_to_obs_colorspace(
		vf->colorspace, vf->color_trc, vf->color_primaries);
	bool is_yuvj = (vf->format == AV_PIX_FMT_YUVJ420P ||
			vf->format == AV_PIX_FMT_YUVJ422P ||
			vf->format == AV_PIX_FMT_YUVJ444P);
	enum video_range_type range =
		(vf->color_range == AVCOL_RANGE_JPEG || is_yuvj)
			? VIDEO_RANGE_FULL
			: VIDEO_RANGE_DEFAULT;

	for (int i = 0; i < plane_count; i++) {
		/* OBS's legacy async frame API requires positive strides. Drop
		 * unusual negative-stride decoded frames instead of converting
		 * them incorrectly and risking an out-of-bounds copy. */
		if (vf->linesize[i] <= 0)
			return;
		frame.data[i] = vf->data[i];
		frame.linesize[i] = (uint32_t)vf->linesize[i];
	}

	frame.format = obs_fmt;
	frame.width = (uint32_t)vf->width;
	frame.height = (uint32_t)vf->height;
	frame.timestamp = out_ts > 0 ? (uint64_t)out_ts : 0;
	frame.flip = false;
	frame.full_range = (range == VIDEO_RANGE_FULL);
	frame.max_luminance = vf->max_luminance;

	switch (vf->color_trc) {
	case AVCOL_TRC_BT709:
	case AVCOL_TRC_GAMMA22:
	case AVCOL_TRC_GAMMA28:
	case AVCOL_TRC_SMPTE170M:
	case AVCOL_TRC_SMPTE240M:
	case AVCOL_TRC_IEC61966_2_1: frame.trc = VIDEO_TRC_SRGB; break;
	case AVCOL_TRC_SMPTE2084: frame.trc = VIDEO_TRC_PQ; break;
	case AVCOL_TRC_ARIB_STD_B67: frame.trc = VIDEO_TRC_HLG; break;
	default: frame.trc = VIDEO_TRC_DEFAULT;
	}

	video_format_get_parameters_for_format(
		space, range, obs_fmt, frame.color_matrix,
		frame.color_range_min, frame.color_range_max);

	obs_source_output_video(s->source, &frame);
}

static void on_audio_frame(void *opaque, struct decoded_audio_frame *af)
{
	struct smooth_media_source *s = opaque;

	if (af->frames == 0 || af->sample_rate == 0)
		return;

	/* The clock tracker coalesces decoded delivery bursts to their final
	 * media timestamp. Skip only startup and true multi-second stalls;
	 * ordinary MPEG-TS batching must remain visible to the estimator. */
	int64_t wall_now = (int64_t)os_gettime_ns();

	if (s->last_audio_arrival_ns != 0) {
		int64_t d_arr = wall_now - s->last_audio_arrival_ns;
		if (d_arr > STALL_GAP_NS) {
			clock_tracker_reset(&s->clock);
			pthread_mutex_lock(&s->controller_mutex);
			s->clock_skip_until_ns = wall_now + CLOCK_SETTLE_NS;
			/* Re-arm the trim: once the post-stall catch-up settles,
			 * drop the stale backlog back to target — jump to live
			 * and restore the jitter headroom instead of carrying
			 * the missed audio as permanent latency. */
			s->did_initial_trim = false;
			pthread_mutex_unlock(&s->controller_mutex);
		}
	}
	s->last_audio_arrival_ns = wall_now;
	s->last_audio_pts_ns = af->pts_ns;

	pthread_mutex_lock(&s->controller_mutex);
	int64_t clock_skip_until_ns = s->clock_skip_until_ns;
	pthread_mutex_unlock(&s->controller_mutex);
	if (wall_now >= clock_skip_until_ns)
		clock_tracker_record(&s->clock, af->pts_ns, wall_now);

	/* Push into the jitter buffer instead of outputting directly.
	 * Audio is drained at a steady pace in smooth_media_tick(),
	 * which prevents bursty network delivery from causing OBS
	 * to accumulate audio buffering. */
	enum audio_format obs_fmt = av_to_obs_audio_format(af->format);
	enum speaker_layout speakers = channels_to_speakers(af->channels);
	if (obs_fmt == AUDIO_FORMAT_UNKNOWN || speakers == SPEAKERS_UNKNOWN)
		return;

	bool was_primed = audio_buffer_is_ready(&s->audio_buf);

	if (!audio_buffer_push(&s->audio_buf, (const uint8_t *const *)af->data,
			       af->data_size, af->frames, af->sample_rate,
			       af->channels, (int)obs_fmt, (int)speakers,
			       af->pts_ns, wall_now)) {
		if (debug_logging_enabled(s))
			SM_LOG(LOG_WARNING, "%s",
			       "Rejected invalid or oversized decoded audio frame");
		return;
	}

	/* Publish first_audio only after a frame has actually entered the
	 * buffer. Unsupported or invalid decoder output must not start a
	 * consumer loop that can never produce audio. */
	if (!os_atomic_load_bool(&s->first_audio)) {
		s->first_audio_pts = af->pts_ns;
		/* Network open/probing can consume much of the nominal warmup
		 * before any media exists. Anchor controller settling to the first
		 * usable audio frame so startup backlog is never mistaken for a
		 * steady-state catch-up opportunity. */
		pthread_mutex_lock(&s->controller_mutex);
		s->stream_start_time = wall_now;
		s->clock_skip_until_ns = wall_now + CLOCK_SETTLE_NS;
		s->did_initial_trim = false;
		pthread_mutex_unlock(&s->controller_mutex);
		os_atomic_set_bool(&s->first_audio, true);
		SM_LOG(LOG_INFO,
		       "First audio frame: %uHz %uch %u samples pts=%lldms",
		       af->sample_rate, af->channels, af->frames,
		       (long long)(af->pts_ns / 1000000));
	}

	if (!was_primed && audio_buffer_is_ready(&s->audio_buf)) {
		SM_LOG(LOG_INFO, "Jitter buffer primed (%lldms buffered)",
		       (long long)(audio_buffer_level_ns(&s->audio_buf) /
				   1000000));
	}

	/* Log if the buffer had to drop frames (overflow).
	 * Batch log at most once per second to avoid spam during
	 * initial bursts (which can produce 80+ drops instantly). */
	struct audio_buffer_stats stats;
	audio_buffer_get_stats(&s->audio_buf, &stats);
	if (stats.frames_dropped > s->last_drop_count) {
		s->pending_drop_count +=
			stats.frames_dropped - s->last_drop_count;
		s->last_drop_count = stats.frames_dropped;

		if (wall_now - s->last_overflow_log_time >= 1000000000LL) {
			SM_LOG(LOG_WARNING,
			       "Audio buffer overflow: dropped %llu frame(s) "
			       "(total dropped: %llu, buffered: %lldms)",
			       (unsigned long long)s->pending_drop_count,
			       (unsigned long long)stats.frames_dropped,
			       (long long)(stats.level_ns / 1000000));
			s->pending_drop_count = 0;
			s->last_overflow_log_time = wall_now;
		}
	}
}

static void on_stream_stopped(void *opaque) { (void)opaque; }

/* FFmpeg's av_strerror can't translate Windows socket (WSA) error codes,
 * so a failed network open just prints "Error number -10049 occurred".
 * Map the common ones to readable, actionable text — IRL streaming hits
 * these constantly. Returns NULL if not a recognized WSA code. */
static const char *winsock_err_str(int averr)
{
	if (averr == INT_MIN)
		return NULL;
	int e = averr < 0 ? -averr : averr;
	switch (e) {
	case 10049:
		return "cannot assign requested address (WSAEADDRNOTAVAIL) — "
		       "the server likely redirected playback to an internal/"
		       "unreachable address, or the app/port is wrong";
	case 10061:
		return "connection refused (WSAECONNREFUSED) — nothing is "
		       "listening on that host:port";
	case 10060:
		return "connection timed out (WSAETIMEDOUT) — host/port "
		       "unreachable or firewalled";
	case 10065: return "no route to host (WSAEHOSTUNREACH)";
	case 10051: return "network unreachable (WSAENETUNREACH)";
	case 10054: return "connection reset by peer (WSAECONNRESET)";
	case 10013: return "permission denied (WSAEACCES)";
	case 11001:
		return "host not found (WSAHOST_NOT_FOUND) — DNS/hostname "
		       "problem";
	default: return NULL;
	}
}

/* Media thread */

static void *media_thread_func(void *data)
{
	struct smooth_media_source *s = data;

	os_set_thread_name("smooth_media_thread");

	int open_err = 0;
	struct stream_decoder_info info = {
		.url = s->url,
		.format_name = s->input_format,
		.ffmpeg_options = s->ffmpeg_options,
		.buffering_bytes = NETWORK_BUFFER_MB * 1024 * 1024,
		.hardware_decoding = s->hw_decode,
		/* Lets stop_media_thread() abort a still-in-progress
		 * avformat_open_input() (e.g. connecting to a dead host) so
		 * closing OBS while the source is connecting/reconnecting
		 * can't hang on pthread_join. */
		.abort_flag = &s->kill,
		.open_result = &open_err,
		.opaque = s,
		.video_cb = on_video_frame,
		.audio_cb = on_audio_frame,
		.stop_cb = on_stream_stopped,
	};

	struct stream_decoder *decoder = stream_decoder_create(&info);
	if (!decoder) {
		os_atomic_set_bool(&s->active, false);
		if (source_is_killed(s))
			return NULL;

		pthread_mutex_lock(&s->state_mutex);
		uint32_t attempts = s->reconnect_attempts;
		pthread_mutex_unlock(&s->state_mutex);

		if (attempts == 0) {
			const char *reason = winsock_err_str(open_err);
			char errbuf[160];
			if (!reason) {
				if (av_strerror(open_err, errbuf,
						sizeof(errbuf)) < 0)
					snprintf(errbuf, sizeof(errbuf),
						 "FFmpeg error %d", open_err);
				reason = errbuf;
			}
			SM_LOG(LOG_WARNING, "Failed to open stream: %s",
			       reason);
		}

		set_media_state(s, OBS_MEDIA_STATE_ENDED);
		os_atomic_set_bool(&s->notify_ended, true);
		return NULL;
	}

	/* Stop may have been requested while avformat_open_input() was
	 * completing. Do not publish a stale PLAYING/started transition. */
	if (source_is_killed(s)) {
		stream_decoder_destroy(decoder);
		os_atomic_set_bool(&s->active, false);
		return NULL;
	}

	pthread_mutex_lock(&s->state_mutex);
	uint32_t attempts = s->reconnect_attempts;
	s->reconnect_attempts = 0;
	pthread_mutex_unlock(&s->state_mutex);

	if (attempts > 0)
		SM_LOG(LOG_INFO, "Reconnected after %u attempts", attempts);
	else
		SM_LOG(LOG_INFO, "%s", "Stream opened");

	set_media_state(s, OBS_MEDIA_STATE_PLAYING);
	os_atomic_set_bool(&s->notify_started, true);

	/* Main decode loop.
	 * Unlike OBS's built-in media source, we do NOT drive timing here.
	 * We just decode as fast as data arrives and push into the jitter
	 * buffer. The clock tracker and jitter buffer handle pacing. */
	while (source_is_active(s) && !source_is_killed(s)) {
		if (!stream_decoder_read_next(decoder)) {
			/* EOF or fatal error */
			break;
		}
	}

	stream_decoder_destroy(decoder);
	os_atomic_set_bool(&s->active, false);

	if (source_is_killed(s))
		return NULL;

	/* Signal end of media */
	obs_source_output_video(s->source, NULL);

	pthread_mutex_lock(&s->state_mutex);
	bool notify_ended = s->state != OBS_MEDIA_STATE_STOPPED;
	if (notify_ended)
		s->state = OBS_MEDIA_STATE_ENDED;
	pthread_mutex_unlock(&s->state_mutex);
	if (notify_ended)
		os_atomic_set_bool(&s->notify_ended, true);

	return NULL;
}

/* Source lifecycle */

/* Caller must hold lifecycle_mutex. */
static void stop_media_thread_locked(struct smooth_media_source *s)
{
	s->reconnecting = false;
	s->reconnect_at_ns = 0;
	os_atomic_set_bool(&s->notify_started, false);
	os_atomic_set_bool(&s->notify_ended, false);

	if (!s->media_thread_valid) {
		os_atomic_set_bool(&s->active, false);
		set_media_state(s, OBS_MEDIA_STATE_STOPPED);
		return;
	}

	/* The decoder interrupt callback observes kill atomically, including
	 * while avformat_open_input() is still blocked. No shared decoder
	 * pointer is touched here, avoiding a teardown use-after-free race. */
	os_atomic_set_bool(&s->kill, true);
	os_atomic_set_bool(&s->active, false);

	pthread_join(s->media_thread, NULL);
	s->media_thread_valid = false;
	/* The media thread may have completed its open in the narrow interval
	 * after kill was set. Suppress any notification it queued before
	 * observing cancellation. */
	os_atomic_set_bool(&s->notify_started, false);
	os_atomic_set_bool(&s->notify_ended, false);
	os_atomic_set_bool(&s->kill, false);
	set_media_state(s, OBS_MEDIA_STATE_STOPPED);
}

static void stop_media_thread(struct smooth_media_source *s)
{
	pthread_mutex_lock(&s->lifecycle_mutex);
	stop_media_thread_locked(s);
	pthread_mutex_unlock(&s->lifecycle_mutex);
}

/* Caller must hold lifecycle_mutex. */
static void start_media_locked(struct smooth_media_source *s)
{
	stop_media_thread_locked(s);

	if (!s->url || !*s->url)
		return;

	/* Reset all state */
	audio_buffer_reset(&s->audio_buf);
	clock_tracker_reset(&s->clock);
	os_atomic_set_bool(&s->first_audio, false);
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
	s->audio_starved = false;
	s->last_underrun_log_time = 0;
	s->stream_start_time = (int64_t)os_gettime_ns();
	s->clock_skip_until_ns = s->stream_start_time + CLOCK_SETTLE_NS;
	s->last_audio_arrival_ns = 0;
	s->last_audio_pts_ns = 0;
	s->did_initial_trim = false;
	s->sr_ratio = 1.0;
	s->playback_ratio = 1.0;
	s->sr_slow_rate = 1.0;
	audio_speed_reset(&s->speed_converter);
	s->last_audio_pop_time = 0;
	s->audio_frame_dur_ns = 0;
	s->prev_video_pts = 0;
	s->video_next_ts = 0;
	s->prev_audio_pts = 0;
	s->audio_next_ts = 0;
	os_atomic_set_bool(&s->active, true);
	os_atomic_set_bool(&s->kill, false);
	s->reconnecting = false;
	s->reconnect_at_ns = 0;

	/* Configure jitter buffer floor; the target/ceiling self-tune from
	 * here based on measured link jitter and underruns. */
	audio_buffer_set_minimum(&s->audio_buf, JITTER_BUFFER_MS * 1000000LL);

	set_media_state(s, OBS_MEDIA_STATE_OPENING);

	if (pthread_create(&s->media_thread, NULL, media_thread_func, s) != 0) {
		SM_LOG(LOG_ERROR, "%s", "Failed to create media thread");
		os_atomic_set_bool(&s->active, false);
		set_media_state(s, OBS_MEDIA_STATE_ERROR);
		os_atomic_set_bool(&s->notify_ended, true);
		return;
	}
	s->media_thread_valid = true;
}

static bool source_should_run_locked(struct smooth_media_source *s)
{
	return s->url && *s->url &&
	       (!s->close_when_inactive || obs_source_showing(s->source));
}

/* Caller holds lifecycle_mutex. The delay is a timestamp, not a sleeping
 * worker. Opening remains asynchronous on the media thread, so the OBS tick
 * never blocks on network I/O. */
static void service_reconnect_locked(struct smooth_media_source *s,
				     int64_t wall_now)
{
	if (!s->reconnecting) {
		s->reconnecting = true;
		s->reconnect_at_ns =
			wall_now +
			(int64_t)s->reconnect_delay_sec * 1000000000LL;

		pthread_mutex_lock(&s->state_mutex);
		uint32_t attempts = s->reconnect_attempts;
		pthread_mutex_unlock(&s->state_mutex);
		if (attempts == 0)
			SM_LOG(LOG_WARNING,
			       "Stream disconnected. Reconnecting every %d seconds...",
			       s->reconnect_delay_sec);
		return;
	}

	if (wall_now < s->reconnect_at_ns)
		return;

	pthread_mutex_lock(&s->state_mutex);
	s->reconnect_attempts++;
	uint32_t attempts = s->reconnect_attempts;
	pthread_mutex_unlock(&s->state_mutex);
	if (attempts == 1 || (attempts % 10) == 0)
		SM_LOG(LOG_INFO, "Reconnect attempt #%u...", attempts);

	s->reconnecting = false;
	s->reconnect_at_ns = 0;
	start_media_locked(s);
}

/* OBS source callbacks */

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
	obs_property_t *hdr =
		obs_properties_add_text(props, "plugin_info",
					"<a href='https://IRLhosting.com'>"
					"IRLhosting Smooth Player</a>"
					" &nbsp;v" SMOOTH_MEDIA_VERSION,
					OBS_TEXT_INFO);
	obs_property_text_set_info_word_wrap(hdr, true);

	/* ── Connection ── */
	obs_properties_t *conn = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_text(conn, "input", "Stream URL",
				    OBS_TEXT_DEFAULT);
	obs_property_set_long_description(
		p, "Full stream URL including protocol.\n"
		   "Examples:\n"
		   "  srt://host:port?streamid=...\n"
		   "  rtmp://host/app/key\n"
		   "  rist://host:port");

	p = obs_properties_add_text(conn, "input_format", "Input Format",
				    OBS_TEXT_DEFAULT);
	obs_property_set_long_description(
		p, "Force a specific container format (e.g. mpegts).\n"
		   "Leave blank for auto-detection.");

	p = obs_properties_add_int_slider(conn, "reconnect_delay_sec",
					  "Reconnect Delay", 1, 60, 1);
	obs_property_int_set_suffix(p, " s");
	obs_property_set_long_description(
		p,
		"Seconds between reconnection attempts when the stream drops.");

	obs_properties_add_group(props, "grp_connection", "Connection",
				 OBS_GROUP_NORMAL, conn);

	/* ── Playback ── */
	obs_properties_t *play = obs_properties_create();

	p = obs_properties_add_bool(play, "hw_decode", "Hardware Decoding");
	obs_property_set_long_description(
		p, "Use GPU-accelerated decoding (NVDEC, QSV, VAAPI).\n"
		   "Reduces CPU load but may not support all codecs.");

	p = obs_properties_add_bool(play, "sync_pts", "Sync A/V via PTS");
	obs_property_set_long_description(
		p, "Use presentation timestamps for audio-video sync.\n"
		   "May help streams with inconsistent frame timing.");

	p = obs_properties_add_bool(play, "close_when_inactive",
				    "Close When Inactive");
	obs_property_set_long_description(
		p, "Stop playback and disconnect when the source is\n"
		   "hidden or on another scene. Saves CPU, bandwidth,\n"
		   "and server resources.");

	p = obs_properties_add_bool(play, "disable_video",
				    "Disable Video Preview (Audio Only)");
	obs_property_set_long_description(
		p, "Stop sending video to OBS while keeping the stream\n"
		   "connected and audio playing. This can reduce rendering\n"
		   "overhead during remote administration. Applies instantly\n"
		   "without reconnecting.");

	obs_properties_add_group(props, "grp_playback", "Playback",
				 OBS_GROUP_NORMAL, play);

	/* ── Advanced ── */
	obs_properties_t *adv = obs_properties_create();

	p = obs_properties_add_text(adv, "ffmpeg_options", "FFmpeg Options",
				    OBS_TEXT_DEFAULT);
	obs_property_set_long_description(
		p, "Extra FFmpeg demuxer/decoder options.\n"
		   "Example: analyzeduration=2000000 probesize=5000000");

	p = obs_properties_add_bool(adv, "debug_logging",
				    "Verbose Debug Logging");
	obs_property_set_long_description(
		p,
		"Log timing data every second, including underruns,\n"
		"dropped frames, and playback-speed correction. Enable only\n"
		"while diagnosing playback. Applies instantly.");

	obs_properties_add_group(props, "grp_advanced", "Advanced",
				 OBS_GROUP_NORMAL, adv);

	return props;
}

static void *smooth_media_create(obs_data_t *settings, obs_source_t *source)
{
	struct smooth_media_source *s =
		bzalloc(sizeof(struct smooth_media_source));
	if (!s)
		return NULL;
	s->source = source;

	if (pthread_mutex_init(&s->lifecycle_mutex, NULL)) {
		bfree(s);
		return NULL;
	}
	if (pthread_mutex_init(&s->state_mutex, NULL)) {
		pthread_mutex_destroy(&s->lifecycle_mutex);
		bfree(s);
		return NULL;
	}
	if (pthread_mutex_init(&s->timing_mutex, NULL)) {
		pthread_mutex_destroy(&s->state_mutex);
		pthread_mutex_destroy(&s->lifecycle_mutex);
		bfree(s);
		return NULL;
	}
	if (pthread_mutex_init(&s->controller_mutex, NULL)) {
		pthread_mutex_destroy(&s->timing_mutex);
		pthread_mutex_destroy(&s->state_mutex);
		pthread_mutex_destroy(&s->lifecycle_mutex);
		bfree(s);
		return NULL;
	}

	if (!audio_buffer_init(&s->audio_buf)) {
		pthread_mutex_destroy(&s->controller_mutex);
		pthread_mutex_destroy(&s->timing_mutex);
		pthread_mutex_destroy(&s->state_mutex);
		pthread_mutex_destroy(&s->lifecycle_mutex);
		bfree(s);
		return NULL;
	}
	if (!clock_tracker_init(&s->clock)) {
		audio_buffer_free(&s->audio_buf);
		pthread_mutex_destroy(&s->controller_mutex);
		pthread_mutex_destroy(&s->timing_mutex);
		pthread_mutex_destroy(&s->state_mutex);
		pthread_mutex_destroy(&s->lifecycle_mutex);
		bfree(s);
		return NULL;
	}
	audio_speed_init(&s->speed_converter);

	/* Apply settings — this will also start the stream */
	smooth_media_update(s, settings);
	return s;
}

static void smooth_media_destroy(void *data)
{
	struct smooth_media_source *s = data;

	stop_media_thread(s);

	audio_buffer_free(&s->audio_buf);
	audio_speed_free(&s->speed_converter);
	clock_tracker_free(&s->clock);

	pthread_mutex_destroy(&s->controller_mutex);
	pthread_mutex_destroy(&s->timing_mutex);
	pthread_mutex_destroy(&s->state_mutex);
	pthread_mutex_destroy(&s->lifecycle_mutex);

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

	const char *url = obs_data_get_string(settings, "input");
	const char *fmt = obs_data_get_string(settings, "input_format");
	const char *opts = obs_data_get_string(settings, "ffmpeg_options");

	char *new_url = (url && *url) ? bstrdup(url) : NULL;
	char *new_fmt = (fmt && *fmt) ? bstrdup(fmt) : NULL;
	char *new_opts = (opts && *opts) ? bstrdup(opts) : NULL;
	bool new_hw = obs_data_get_bool(settings, "hw_decode");
	if (((url && *url) && !new_url) || ((fmt && *fmt) && !new_fmt) ||
	    ((opts && *opts) && !new_opts)) {
		SM_LOG(LOG_ERROR, "%s",
		       "Unable to apply settings: out of memory");
		bfree(new_url);
		bfree(new_fmt);
		bfree(new_opts);
		return;
	}

	pthread_mutex_lock(&s->lifecycle_mutex);

	/* Only the settings that affect the decoder pipeline force a
	 * reconnect. Playback-only toggles (sync_pts, disable_video,
	 * close_when_inactive, reconnect delay) apply live so flipping
	 * them never interrupts the stream. */
	bool stream_changed = str_changed(s->url, new_url) ||
			      str_changed(s->input_format, new_fmt) ||
			      str_changed(s->ffmpeg_options, new_opts) ||
			      s->hw_decode != new_hw;

	/* Stop before releasing configuration strings. The media thread reads
	 * them while constructing/logging its decoder, so freeing first was a
	 * use-after-free risk under concurrent updates. */
	if (stream_changed)
		stop_media_thread_locked(s);

	bfree(s->url);
	bfree(s->input_format);
	bfree(s->ffmpeg_options);
	s->url = new_url;
	s->input_format = new_fmt;
	s->ffmpeg_options = new_opts;
	s->hw_decode = new_hw;

	s->sync_pts = obs_data_get_bool(settings, "sync_pts");
	s->close_when_inactive =
		obs_data_get_bool(settings, "close_when_inactive");
	bool disable_video = obs_data_get_bool(settings, "disable_video");
	os_atomic_set_bool(&s->disable_video, disable_video);
	os_atomic_set_bool(&s->debug_logging,
			   obs_data_get_bool(settings, "debug_logging"));
	s->reconnect_delay_sec =
		(int)obs_data_get_int(settings, "reconnect_delay_sec");

	if (s->reconnect_delay_sec < 1)
		s->reconnect_delay_sec = 10;

	bool should_run = source_should_run_locked(s);

	if (!s->url || !*s->url) {
		stop_media_thread_locked(s);
	} else if (stream_changed) {
		if (should_run)
			start_media_locked(s);
		else
			stop_media_thread_locked(s);
	} else if (should_run && !source_is_active(s) &&
		   !s->media_thread_valid) {
		start_media_locked(s);
	} else if (!should_run && (source_is_active(s) ||
				   s->media_thread_valid || s->reconnecting)) {
		stop_media_thread_locked(s);
	}

	pthread_mutex_unlock(&s->lifecycle_mutex);

	/* Blank the preview immediately when switching to audio-only. */
	if (disable_video)
		obs_source_output_video(s->source, NULL);
}

static void smooth_media_activate(void *data)
{
	struct smooth_media_source *s = data;
	pthread_mutex_lock(&s->lifecycle_mutex);
	if (!s->close_when_inactive && s->url && *s->url &&
	    !source_is_active(s))
		start_media_locked(s);
	pthread_mutex_unlock(&s->lifecycle_mutex);
}

static void smooth_media_deactivate(void *data)
{
	struct smooth_media_source *s = data;
	pthread_mutex_lock(&s->lifecycle_mutex);
	bool should_stop = s->close_when_inactive;
	if (should_stop)
		stop_media_thread_locked(s);
	pthread_mutex_unlock(&s->lifecycle_mutex);
	if (should_stop)
		obs_source_output_video(s->source, NULL);
}

static void smooth_media_show(void *data)
{
	struct smooth_media_source *s = data;
	pthread_mutex_lock(&s->lifecycle_mutex);
	if (s->close_when_inactive && s->url && *s->url && !source_is_active(s))
		start_media_locked(s);
	pthread_mutex_unlock(&s->lifecycle_mutex);
}

static void smooth_media_hide(void *data)
{
	struct smooth_media_source *s = data;
	pthread_mutex_lock(&s->lifecycle_mutex);
	bool should_stop = s->close_when_inactive;
	if (should_stop)
		stop_media_thread_locked(s);
	pthread_mutex_unlock(&s->lifecycle_mutex);
	if (should_stop)
		obs_source_output_video(s->source, NULL);
}

static void smooth_media_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct smooth_media_source *s = data;
	pthread_mutex_lock(&s->lifecycle_mutex);

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
	if (source_is_active(s) && os_atomic_load_bool(&s->first_audio)) {
		int64_t wall_now = (int64_t)os_gettime_ns();
		bool debug = debug_logging_enabled(s);
		pthread_mutex_lock(&s->timing_mutex);
		uint64_t video_frames_snapshot = s->video_frames_out;
		pthread_mutex_unlock(&s->timing_mutex);
		int64_t frame_dur = s->audio_frame_dur_ns;
		if (frame_dur <= 0)
			frame_dur = 21333333LL; /* 1024 / 48000 */

		/* Measured stream delivery rate (clamped to sane drift). */
		double rate = clock_tracker_get_smoothed_rate(&s->clock);
		if (rate != rate)
			rate = 1.0;
		else if (rate < 0.90)
			rate = 0.90;
		else if (rate > 1.10)
			rate = 1.10;

		/* One-shot: once the connect burst has settled, discard its
		 * stale backlog down to target so we start near live with the
		 * full jitter margin available — otherwise the burst leaves the
		 * buffer pinned near max (≈300ms) for the whole session. */
		bool buffer_ready = audio_buffer_is_ready(&s->audio_buf);
		pthread_mutex_lock(&s->controller_mutex);
		bool should_trim = !s->did_initial_trim &&
				   wall_now >= s->clock_skip_until_ns &&
				   buffer_ready;
		if (should_trim)
			s->did_initial_trim = true;
		pthread_mutex_unlock(&s->controller_mutex);
		if (should_trim) {
			int64_t before = audio_buffer_level_ns(&s->audio_buf);
			audio_buffer_trim_to_target(&s->audio_buf);
			if (debug)
				SM_LOG(LOG_INFO,
				       "DBG initial trim: %lldms -> %lldms",
				       (long long)(before / 1000000),
				       (long long)(audio_buffer_level_ns(
							   &s->audio_buf) /
						   1000000));
		}

		/* ── Closed-loop playback pacing ──
		 * The media-clock estimate supplies the long-term pace. Buffer
		 * occupancy adds a bounded correction: underfill gently slows
		 * playback; recovery backlog gently speeds it up. The exact same
		 * ratio drives pop cadence, audio conversion, and video PTS. */
		int64_t target = audio_buffer_target_ns(&s->audio_buf);
		int64_t level = audio_buffer_level_ns(&s->audio_buf);
		double lvl_err = 0.0;
		if (target > 0) {
			lvl_err = (double)(level - target) / (double)target;
			if (lvl_err < -0.5)
				lvl_err = -0.5;
			if (lvl_err > 0.5)
				lvl_err = 0.5;
		}

		/* Smooth the long-term estimate, then combine it with the
		 * occupancy correction. Underfill gets more authority than
		 * overfill: avoiding silence is worth a larger tempo change,
		 * while recovery catch-up should remain subtle. */
		s->sr_slow_rate += SR_SLOW_ALPHA * (rate - s->sr_slow_rate);
		pthread_mutex_lock(&s->controller_mutex);
		int64_t controller_start_time = s->stream_start_time;
		pthread_mutex_unlock(&s->controller_mutex);
		bool in_warmup =
			(wall_now - controller_start_time) < SR_WARMUP_NS;
		double base_ratio = in_warmup ? 1.0 : s->sr_slow_rate;
		double occupancy_gain =
			lvl_err < 0.0 ? 0.15 : (in_warmup ? 0.0 : 0.08);
		double desired_ratio =
			base_ratio * (1.0 + occupancy_gain * lvl_err);
		if (desired_ratio < PLAYBACK_RATIO_MIN)
			desired_ratio = PLAYBACK_RATIO_MIN;
		if (desired_ratio > PLAYBACK_RATIO_MAX)
			desired_ratio = PLAYBACK_RATIO_MAX;
		pthread_mutex_lock(&s->controller_mutex);
		if (s->sr_ratio <= 0.0)
			s->sr_ratio = 1.0;
		double pacing_ratio = s->sr_ratio;
		pthread_mutex_unlock(&s->controller_mutex);
		int64_t pop_interval =
			(int64_t)((double)frame_dur / pacing_ratio);

		int pops = 0;
		/* Cap at 8 so a post-stall backlog can be drained over a tick
		 * without an unbounded burst. */
		while (pops < 8) {
			bool should_pop;
			if (s->last_audio_pop_time == 0) {
				should_pop =
					audio_buffer_is_ready(&s->audio_buf);
			} else {
				int64_t elapsed =
					wall_now - s->last_audio_pop_time;
				should_pop = (elapsed >= pop_interval);
			}
			if (!should_pop)
				break;

			struct audio_buf_frame *buf_frame;
			if (!audio_buffer_pop(&s->audio_buf, &buf_frame)) {
				/* Wanted audio but buffer empty → underrun;
				 * grow the adaptive cushion so we rebuild a
				 * deeper buffer after the stall. */
				if (s->audio_frames_out > 0 &&
				    !s->audio_starved) {
					audio_buffer_note_underrun(
						&s->audio_buf);
					s->underrun_count++;
					s->audio_starved = true;
					if (debug &&
					    (wall_now -
					     s->last_underrun_log_time) >=
						    500000000LL) {
						SM_LOG(LOG_INFO,
						       "DBG underrun #%llu (buf "
						       "empty, tgt now %lldms)",
						       (unsigned long long)s
							       ->underrun_count,
						       (long long)(audio_buffer_target_ns(
									   &s->audio_buf) /
								   1000000));
						s->last_underrun_log_time =
							wall_now;
					}
				}
				break;
			}
			s->audio_starved = false;

			/* Update frame duration estimate from actual
			 * popped frame */
			if (buf_frame->sample_rate > 0 && buf_frame->frames > 0)
				s->audio_frame_dur_ns =
					(int64_t)buf_frame->frames *
					1000000000LL / buf_frame->sample_rate;

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

			pthread_mutex_lock(&s->controller_mutex);
			/* Slew gently in normal operation. If startup reserve has
			 * fallen below half target, slowing down promptly is safer
			 * than letting the queue empty and producing silence. */
			double slew = in_warmup && lvl_err < -0.25 &&
					      desired_ratio < s->sr_ratio
				      ? SR_EMERGENCY_SLEW_PER_POP
				      : SR_SLEW_PER_POP;
			double dr = desired_ratio - s->sr_ratio;
			if (dr > slew)
				dr = slew;
			if (dr < -slew)
				dr = -slew;
			s->sr_ratio += dr;
			double sr_ratio = s->sr_ratio;
			pthread_mutex_unlock(&s->controller_mutex);

			enum AVSampleFormat av_format =
				obs_to_av_audio_format(buf_frame->format);
			struct audio_speed_frame speed_frame = {0};
			bool speed_converted =
				av_format != AV_SAMPLE_FMT_NONE &&
				audio_speed_convert(
					&s->speed_converter,
					(const uint8_t *const *)buf_frame->data,
					buf_frame->frames,
					(int)buf_frame->sample_rate,
					(int)buf_frame->channels, av_format,
					sr_ratio, &speed_frame);
			uint32_t output_frames = speed_converted
							 ? speed_frame.frames
							 : buf_frame->frames;
			uint32_t output_sample_rate = buf_frame->sample_rate;
			pthread_mutex_lock(&s->controller_mutex);
			s->playback_ratio = speed_converted ? sr_ratio : 1.0;
			pthread_mutex_unlock(&s->controller_mutex);

			/* ── Timestamp computation ──
			 * Timestamp spacing must match the duration OBS will
			 * actually consume after speed conversion. */
			int64_t audio_step = (int64_t)output_frames *
					     1000000000LL /
					     (int64_t)output_sample_rate;
			int64_t out_ts;
			if (s->audio_frames_out == 0) {
				s->audio_next_ts = wall_now;
				out_ts = wall_now;
			} else {
				s->audio_next_ts += audio_step;
				int64_t drift_error =
					wall_now - s->audio_next_ts;
				bool tight_sync = s->sync_pts &&
						  video_frames_snapshot > 0;

				if (drift_error < 0)
					s->audio_next_ts += drift_error / 10;
				else if (tight_sync ||
					 drift_error > 100000000LL)
					s->audio_next_ts += drift_error / 100;
				else
					s->audio_next_ts += drift_error / 1000;

				int64_t max_ahead =
					tight_sync ? wall_now
						   : wall_now + 20000000LL;
				if (s->audio_next_ts > max_ahead)
					s->audio_next_ts = max_ahead;
				if (s->audio_next_ts < wall_now - 200000000LL)
					s->audio_next_ts = wall_now;
				out_ts = s->audio_next_ts;
			}
			s->prev_audio_pts = buf_frame->pts_ns;
			if (s->audio_frames_out > 0 &&
			    out_ts <= s->audio_out_ts)
				out_ts = s->audio_out_ts + 1;
			s->audio_next_ts = out_ts;
			s->audio_out_ts = out_ts;

			/* ── Output to OBS ── */
			struct obs_source_audio obs_audio = {0};
			for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++) {
				if (speed_converted) {
					obs_audio.data[i] =
						speed_frame.data_size[i] > 0
							? speed_frame.data[i]
							: NULL;
				} else {
					obs_audio.data[i] =
						buf_frame->data_size[i] > 0
							? buf_frame->data[i]
							: NULL;
				}
			}

			obs_audio.frames = output_frames;
			obs_audio.samples_per_sec = output_sample_rate;
			obs_audio.format = (enum audio_format)buf_frame->format;
			obs_audio.speakers =
				(enum speaker_layout)buf_frame->speakers;
			obs_audio.timestamp = out_ts > 0 ? (uint64_t)out_ts : 0;

			/* One-time confirmation of exactly what we hand OBS, so
			 * "no audio" reports can be triaged: if these look sane
			 * and the mixer meter still doesn't move, the issue is
			 * OBS-side routing (mute / track / monitoring). */
			if (s->audio_frames_out == 0)
				SM_LOG(LOG_INFO,
				       "First audio -> OBS: fmt=%d speakers=%d "
				       "sr=%u frames=%u ts=%lldms",
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
			int64_t diag_interval =
				debug ? 1000000000LL : 5000000000LL;
			if ((wall_now - s->last_diag_time) >= diag_interval) {
				struct audio_buffer_stats stats;
				audio_buffer_get_stats(&s->audio_buf, &stats);
				pthread_mutex_lock(&s->timing_mutex);
				video_frames_snapshot = s->video_frames_out;
				int64_t video_ts_snapshot = s->video_out_ts;
				pthread_mutex_unlock(&s->timing_mutex);
				int64_t av_wall = 0;
				if (video_frames_snapshot > 0)
					av_wall = s->audio_out_ts -
						  video_ts_snapshot;
				if (debug) {
					SM_LOG(LOG_INFO,
					       "DBG rate=%.4f slow=%.4f "
					       "speed=%.5f out_sr=%u "
					       "buf=%lldms tgt=%lldms jit=%lldms "
					       "gap=%lldms "
					       "pop_iv=%lldms "
					       "av=%lldms a=%llu v=%llu drop=%llu "
					       "under=%llu%s",
					       rate, s->sr_slow_rate,
					       s->sr_ratio, output_sample_rate,
					       (long long)(stats.level_ns /
							   1000000),
					       (long long)(stats.target_ns /
							   1000000),
					       (long long)(stats.jitter_ns /
							   1000000),
					       (long long)(stats.delivery_gap_ns /
							   1000000),
					       (long long)(pop_interval /
							   1000000),
					       (long long)(av_wall / 1000000),
					       (unsigned long long)
						       s->audio_frames_out,
					       (unsigned long long)
						       video_frames_snapshot,
					       (unsigned long long)
						       stats.frames_dropped,
					       (unsigned long long)
						       s->underrun_count,
					       s->sync_pts ? " [PTS-SYNC]"
							   : "");
				} else {
					SM_LOG(LOG_INFO,
					       "DIAG: rate=%.4f speed=%.4f sr=%u "
					       "buf=%lldms tgt=%lldms "
					       "av_wall=%lldms "
					       "a_out=%llu v_out=%llu%s",
					       rate, s->sr_ratio,
					       output_sample_rate,
					       (long long)(stats.level_ns /
							   1000000),
					       (long long)(stats.target_ns /
							   1000000),
					       (long long)(av_wall / 1000000),
					       (unsigned long long)
						       s->audio_frames_out,
					       (unsigned long long)
						       video_frames_snapshot,
					       s->sync_pts ? " [PTS-SYNC]"
							   : "");
				}
				s->last_diag_time = wall_now;
			}
		}
	}

	/* Check if media ended and needs reconnect. With
	 * close_when_inactive=OFF, the source is explicitly expected to stay
	 * connected even while hidden/inactive. */
	if (!source_is_active(s) && source_should_run_locked(s) &&
	    get_media_state(s) == OBS_MEDIA_STATE_ENDED)
		service_reconnect_locked(s, (int64_t)os_gettime_ns());

	bool notify_started =
		os_atomic_exchange_bool(&s->notify_started, false);
	bool notify_ended = os_atomic_exchange_bool(&s->notify_ended, false);
	pthread_mutex_unlock(&s->lifecycle_mutex);

	/* OBS media signals are synchronous. Emit them without our lifecycle
	 * lock so a signal handler that queries this source cannot deadlock. */
	if (notify_started)
		obs_source_media_started(s->source);
	if (notify_ended)
		obs_source_media_ended(s->source);
}

/* Media control callbacks */
static void smooth_media_restart(void *data)
{
	struct smooth_media_source *s = data;
	pthread_mutex_lock(&s->lifecycle_mutex);
	if (source_should_run_locked(s))
		start_media_locked(s);
	pthread_mutex_unlock(&s->lifecycle_mutex);
}

static void smooth_media_stop(void *data)
{
	struct smooth_media_source *s = data;
	stop_media_thread(s);
	obs_source_output_video(s->source, NULL);
}

static enum obs_media_state smooth_media_get_state(void *data)
{
	struct smooth_media_source *s = data;
	return get_media_state(s);
}

bool smooth_media_get_status_snapshot(struct smooth_media_source *s,
				      struct smooth_media_status *status)
{
	if (!s || !status)
		return false;
	memset(status, 0, sizeof(*status));

	pthread_mutex_lock(&s->lifecycle_mutex);
	if (s->url) {
		status->url = bstrdup(s->url);
		if (!status->url) {
			pthread_mutex_unlock(&s->lifecycle_mutex);
			return false;
		}
	}
	status->active = source_is_active(s);
	status->reconnecting = s->reconnecting;
	status->audio_frames_out = s->audio_frames_out;
	int64_t audio_ts = s->audio_out_ts;
	status->state = get_media_state(s);

	pthread_mutex_lock(&s->timing_mutex);
	status->video_frames_out = s->video_frames_out;
	int64_t video_ts = s->video_out_ts;
	pthread_mutex_unlock(&s->timing_mutex);

	if (audio_ts && video_ts)
		status->av_offset_ms = (audio_ts - video_ts) / 1000000;
	pthread_mutex_unlock(&s->lifecycle_mutex);
	return true;
}

void smooth_media_status_free(struct smooth_media_status *status)
{
	if (!status)
		return;
	bfree(status->url);
	memset(status, 0, sizeof(*status));
}

/* OBS source registration */

struct obs_source_info smooth_media_source_info = {
	.id = "smooth_media_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO |
			OBS_SOURCE_DO_NOT_DUPLICATE |
			OBS_SOURCE_CONTROLLABLE_MEDIA,
	.get_name = smooth_media_get_name,
	.create = smooth_media_create,
	.destroy = smooth_media_destroy,
	.get_defaults = smooth_media_defaults,
	.get_properties = smooth_media_get_properties,
	.activate = smooth_media_activate,
	.deactivate = smooth_media_deactivate,
	.show = smooth_media_show,
	.hide = smooth_media_hide,
	.video_tick = smooth_media_tick,
	.update = smooth_media_update,
	.icon_type = OBS_ICON_TYPE_MEDIA,
	.media_restart = smooth_media_restart,
	.media_stop = smooth_media_stop,
	.media_get_state = smooth_media_get_state,
};
