#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Audio Jitter Buffer — absorbs timing irregularities from live streams.
 *
 * Instead of forwarding audio frames immediately (which causes bursting
 * when the stream is slow), this buffer collects frames and releases
 * them at a steady pace controlled by the clock tracker.
 *
 * Key behaviors:
 * - Accumulates audio frames with their PTS
 * - Only releases frames when buffer level is above a minimum threshold
 * - If buffer runs low, holds frames (no output) rather than outputting
 *   with bad timing — the stream will catch up
 * - If buffer grows too large, drops oldest frames to prevent unbounded
 *   latency growth
 */

#define AUDIO_BUF_MAX_PLANES 8
#define AUDIO_BUF_MAX_FRAMES 128

struct audio_buf_frame {
	uint8_t *data[AUDIO_BUF_MAX_PLANES];
	uint32_t frames;          /* number of audio samples */
	uint32_t sample_rate;
	uint32_t channels;
	int64_t  pts_ns;          /* stream PTS in nanoseconds */
	int      format;          /* obs audio_format enum value */
	int      speakers;        /* obs speaker_layout enum value */
	size_t   data_size[AUDIO_BUF_MAX_PLANES];
	bool     valid;
};

struct audio_buffer {
	struct audio_buf_frame frames[AUDIO_BUF_MAX_FRAMES];
	int read_pos;
	int write_pos;
	int count;

	/* Configuration */
	int64_t min_buffer_ns;    /* minimum buffer level before we start outputting (default: 80ms) */
	int64_t max_buffer_ns;    /* maximum buffer level before we drop old frames (default: 500ms) */

	/* State */
	bool primed;              /* true once we've accumulated min_buffer_ns of data */
	int64_t total_buffered_ns; /* approximate total buffered duration */
	int64_t last_output_pts;  /* PTS of last output frame */

	/* Stats */
	uint64_t frames_in;
	uint64_t frames_out;
	uint64_t frames_dropped;
};

void audio_buffer_init(struct audio_buffer *ab);
void audio_buffer_free(struct audio_buffer *ab);
void audio_buffer_reset(struct audio_buffer *ab);

/* Push an audio frame into the buffer. Data is copied. */
bool audio_buffer_push(struct audio_buffer *ab, const uint8_t *const *data,
		       const size_t *data_sizes, uint32_t frames,
		       uint32_t sample_rate, uint32_t channels,
		       int format, int speakers, int64_t pts_ns);

/* Try to pop the next audio frame. Returns false if buffer isn't ready.
 * The returned frame pointer is valid until the next push or pop. */
bool audio_buffer_pop(struct audio_buffer *ab, struct audio_buf_frame **out);

/* Get current buffer level in nanoseconds */
int64_t audio_buffer_level_ns(const struct audio_buffer *ab);

/* Check if buffer is primed and ready to output */
bool audio_buffer_is_ready(const struct audio_buffer *ab);

/* Get buffer fill percentage (0.0 - 1.0) */
double audio_buffer_fill_ratio(const struct audio_buffer *ab);
