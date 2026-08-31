#pragma once

#define SMOOTH_MEDIA_VERSION "1.4.23"

#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include "audio-buffer.h"
#include "audio-speed.h"
#include "clock-tracker.h"
#include "output-timeline.h"
#include "stream-decoder.h"

/*
 * Smooth Media Source — OBS source plugin for smooth RTMP/SRT/RIST playback.
 *
 * This source does its own FFmpeg demuxing and uses clock drift tracking
 * plus an audio jitter buffer to eliminate the audio stutter that occurs
 * when live streams deliver data slightly slower than realtime.
 */

struct smooth_media_source {
	obs_source_t *source;

	/* Settings */
	char *url;
	char *input_format;
	char *ffmpeg_options;
	bool hw_decode;
	volatile bool sync_pts;
	bool adaptive_audio_speed;
	bool close_when_inactive;
	volatile bool
		disable_video; /* audio-only: skip video output (eases remote testing) */
	volatile bool
		debug_logging; /* verbose per-second diagnostics + event logging */
	int reconnect_delay_sec;

	/* Audio buffering and clock tracking */
	struct audio_buffer audio_buf;
	struct audio_speed_converter speed_converter;
	struct clock_tracker clock;
	struct output_timeline audio_timeline;

	/* Playback state */
	pthread_mutex_t lifecycle_mutex;
	pthread_t media_thread;
	bool media_thread_valid;
	volatile bool active;
	volatile bool kill;
	volatile bool notify_started;
	volatile bool notify_ended;

	/* Reconnection is scheduled by video_tick; opening still happens on the
	 * media thread. This avoids a second worker thread mutating source
	 * lifecycle state and resetting buffers concurrently with OBS. */
	bool reconnecting;
	int64_t reconnect_at_ns;
	uint32_t reconnect_attempts; /* suppress repeated failure logs */

	/* Timestamp tracking for output */
	int64_t audio_out_ts; /* last audio output timestamp */
	int64_t video_out_ts; /* last video output timestamp */
	int64_t audio_sync_pts; /* media PTS for the latest audio timing anchor */
	int64_t audio_sync_ts;  /* OBS timestamp paired with audio_sync_pts */
	int64_t video_media_pts; /* media PTS for the latest video output */
	bool audio_sync_valid;
	volatile bool first_audio;
	bool first_video;
	bool got_first_keyframe;
	int64_t first_audio_pts;
	int64_t first_video_pts;
	int64_t prev_video_pts;    /* previous video PTS for delta stepping */
	int64_t video_next_ts;     /* monotonic video output timestamp */
	int64_t prev_audio_pts;    /* previous audio PTS for discontinuity checks */
	uint64_t audio_frames_out; /* counter for periodic diagnostics */
	uint64_t video_frames_out; /* counter for periodic diagnostics */
	uint64_t last_drop_count;  /* for detecting new drops */
	int64_t last_overflow_log_time; /* wall time of last overflow log */
	uint64_t pending_drop_count;    /* drops since last overflow log */
	int64_t last_diag_time;         /* wall clock of last diagnostic log */
	uint64_t underrun_count;        /* total drain underruns (debug) */
	uint64_t recovery_count; /* completed controlled rebuffer cycles */
	volatile bool audio_rebuffering; /* hold A/V until recovery reserve exists */
	bool video_reanchor_pending; /* align video to a recovered audio epoch */
	int64_t stream_start_time;      /* wall clock when stream opened */
	int64_t clock_skip_until_ns; /* skip rate measurement until this wall time
                                     (connect/stall settle) */
	int64_t last_audio_arrival_ns; /* wall time of previous audio frame (stall
                                     detection) */
	bool did_initial_trim; /* one-shot: trim the connect burst backlog to target */
	double sr_ratio;       /* slew-limited in-plugin playback ratio */
	double playback_ratio; /* in-plugin audio speed shared with video pacing */
	double sr_slow_rate;         /* smoothed media-clock estimate */
	int64_t last_audio_pop_time; /* wall time of last audio pop (for steady-rate
                                  drain) */
	int64_t audio_frame_dur_ns; /* estimated duration of one audio frame in ns */

	/* State */
	enum obs_media_state state;
	pthread_mutex_t state_mutex;
	pthread_mutex_t timing_mutex;
	pthread_mutex_t controller_mutex;
};

struct smooth_media_status {
	enum obs_media_state state;
	bool active;
	bool reconnecting;
	char *url;
	uint64_t audio_frames_out;
	uint64_t video_frames_out;
	int64_t av_offset_ms;
};

bool smooth_media_get_status_snapshot(struct smooth_media_source *s,
				      struct smooth_media_status *status);
void smooth_media_status_free(struct smooth_media_status *status);

/* OBS source info registration */
extern struct obs_source_info smooth_media_source_info;
