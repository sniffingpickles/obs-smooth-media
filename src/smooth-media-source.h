#pragma once

#define SMOOTH_MEDIA_VERSION "1.4.8"

#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>

#include "stream-decoder.h"
#include "audio-buffer.h"
#include "clock-tracker.h"

/*
 * Smooth Media Source — OBS source plugin for stutter-free RTMP/SRT playback.
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
	bool sync_pts;
	bool close_when_inactive;
	volatile bool disable_video; /* audio-only: skip video output (eases remote testing) */
	volatile bool debug_logging; /* verbose per-second diagnostics + event logging */
	int reconnect_delay_sec;

	/* Decoder */
	struct stream_decoder *decoder;

	/* Audio buffering and clock tracking */
	struct audio_buffer audio_buf;
	struct clock_tracker clock;

	/* Playback state */
	pthread_t media_thread;
	bool media_thread_valid;
	volatile bool active;
	volatile bool kill;

	/* Reconnection */
	pthread_t reconnect_thread;
	pthread_mutex_t reconnect_mutex;
	bool reconnect_thread_valid;
	os_event_t *reconnect_stop_event;
	volatile bool reconnecting;
	uint32_t reconnect_attempts;   /* suppress repeated failure logs */

	/* Timestamp tracking for output */
	int64_t audio_out_ts;      /* last audio output timestamp */
	int64_t video_out_ts;      /* last video output timestamp */
	bool first_audio;
	bool first_video;
	bool got_first_keyframe;
	int64_t first_audio_pts;
	int64_t first_video_pts;
	int64_t prev_video_pts;        /* previous video PTS for delta stepping */
	int64_t video_next_ts;         /* monotonic video output timestamp */
	int64_t prev_audio_pts;        /* previous audio PTS for delta stepping */
	int64_t audio_next_ts;         /* monotonic audio output timestamp */
	uint64_t audio_frames_out;     /* counter for periodic diagnostics */
	uint64_t video_frames_out;     /* counter for periodic diagnostics */
	uint64_t last_drop_count;      /* for detecting new drops */
	int64_t last_overflow_log_time; /* wall time of last overflow log */
	uint64_t pending_drop_count;   /* drops since last overflow log */
	int64_t last_diag_time;        /* wall clock of last diagnostic log */
	uint64_t underrun_count;       /* total drain underruns (debug) */
	uint64_t sr_change_count;      /* total declared-sample-rate changes (debug) */
	int64_t last_underrun_log_time; /* rate-limit for debug underrun logs */
	int64_t stream_start_time;     /* wall clock when stream opened */
	int64_t clock_skip_until_ns;   /* skip rate measurement until this wall time (connect/stall settle) */
	int64_t last_audio_arrival_ns; /* wall time of previous audio frame (stall/burst detection) */
	int64_t last_audio_pts_ns;     /* PTS of previous audio frame (delivery-rate estimate) */
	double deliv_ema;              /* smoothed instantaneous delivery rate (pts/wall); ~1 steady, >1 catch-up burst, <1 stall */
	int64_t overfill_since_ns;     /* wall time the buffer first went well above target (0 = at/near target) */
	bool did_initial_trim;         /* one-shot: trim the connect burst backlog to target */
	double sr_ratio;               /* slew-limited declared-sample-rate ratio (1.0 = no correction) */
	double sr_slow_rate;           /* heavily-smoothed drift estimate that drives the declared rate */
	uint32_t declared_sr;          /* last sample rate handed to OBS (held stable via deadband) */
	int64_t last_sr_change_ns;     /* wall time the declared rate last changed (min-hold) */
	int64_t last_audio_pop_time;   /* wall time of last audio pop (for steady-rate drain) */
	int64_t audio_frame_dur_ns;    /* estimated duration of one audio frame in ns */

	/* State */
	enum obs_media_state state;
	pthread_mutex_t state_mutex;
};

/* OBS source info registration */
extern struct obs_source_info smooth_media_source_info;
