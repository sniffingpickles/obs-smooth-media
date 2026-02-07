#pragma once

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
	volatile bool stopping;
	volatile bool kill;

	/* Reconnection */
	pthread_t reconnect_thread;
	pthread_mutex_t reconnect_mutex;
	bool reconnect_thread_valid;
	os_event_t *reconnect_stop_event;
	volatile bool reconnecting;

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
	uint64_t audio_frames_out;     /* counter for periodic diagnostics */
	uint64_t video_frames_out;     /* counter for periodic diagnostics */
	uint64_t last_drop_count;      /* for detecting new drops */
	int64_t last_diag_time;        /* wall clock of last diagnostic log */
	int64_t stream_start_time;     /* wall clock when stream opened */
	int64_t sr_hold_start;         /* wall time when rate first left deadzone (0=inside) */

	/* State */
	enum obs_media_state state;
	pthread_mutex_t state_mutex;
};

/* OBS source info registration */
extern struct obs_source_info smooth_media_source_info;
