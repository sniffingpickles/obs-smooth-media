#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <util/threading.h>

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
/*
 * 512 slots still keeps the structure modest, but unlike the old 128-slot
 * ring it can reach the 750 ms adaptive target with small codec frames
 * (for example 120 samples / 2.5 ms at 48 kHz).
 */
#define AUDIO_BUF_MAX_FRAMES 512
#define AUDIO_BUF_MAX_FRAME_BYTES (4U * 1024U * 1024U)

struct audio_buf_frame {
	uint8_t *data[AUDIO_BUF_MAX_PLANES];
	uint32_t frames; /* number of audio samples */
	uint32_t sample_rate;
	uint32_t channels;
	int64_t pts_ns; /* stream PTS in nanoseconds */
	int format;     /* obs audio_format enum value */
	int speakers;   /* obs speaker_layout enum value */
	size_t data_size[AUDIO_BUF_MAX_PLANES];
	size_t data_capacity[AUDIO_BUF_MAX_PLANES];
	bool valid;
};

struct audio_buffer {
	pthread_mutex_t mutex;
	struct audio_buf_frame frames[AUDIO_BUF_MAX_FRAMES];
	int read_pos;
	int write_pos;
	int count;

	/* Configuration */
	int64_t min_buffer_ns; /* floor for the adaptive target (default: 80ms) */
	int64_t max_buffer_ns; /* hard latency ceiling; drop old frames past this */

	/* Adaptive jitter target — self-tuning, no user knobs. Grows with the
	 * measured arrival jitter and with recent underruns; decays slowly when
	 * the link is calm. The consumer primes at, and paces toward, this. */
	int64_t target_buffer_ns;   /* current adaptive target depth */
	int64_t jitter_ns;          /* RFC 3550-style smoothed arrival jitter */
	int64_t underrun_credit_ns; /* extra cushion accrued after underruns */
	int64_t delivery_gap_ns;    /* recent peak decoded-frame delivery gap */
	int64_t last_gap_decay_ns;  /* wall time used for peak-gap decay */
	int64_t prev_arrival_ns;    /* wall time of previous push */
	int64_t prev_arrival_pts_ns; /* PTS of previous push */
	bool have_arrival_ref;       /* prev_arrival_* are valid */

	/* State */
	bool primed; /* true once we've accumulated target_buffer_ns of data */
	int64_t total_buffered_ns; /* approximate total buffered duration */
	int64_t last_output_pts;   /* PTS of last output frame */

	/* Single-consumer staging frame. audio_buffer_pop() swaps the popped
	 * ring slot into this under the mutex and returns a pointer to it, so
	 * the consumer can hand the data to OBS without allocation/copying or
	 * a concurrent producer push corrupting it. */
	struct audio_buf_frame out_frame;

	/* Stats */
	uint64_t frames_in;
	uint64_t frames_out;
	uint64_t frames_dropped;
};

struct audio_buffer_stats {
	int count;
	bool primed;
	int64_t level_ns;
	int64_t target_ns;
	int64_t max_ns;
	int64_t jitter_ns;
	int64_t delivery_gap_ns;
	uint64_t frames_in;
	uint64_t frames_out;
	uint64_t frames_dropped;
};

/* Returns false if the internal mutex could not be created. */
bool audio_buffer_init(struct audio_buffer *ab);
void audio_buffer_free(struct audio_buffer *ab);
void audio_buffer_reset(struct audio_buffer *ab);

/* Set the adaptive buffer's minimum target. Values are clamped to the
 * controller's supported range. */
void audio_buffer_set_minimum(struct audio_buffer *ab, int64_t minimum_ns);

/* Push an audio frame into the buffer. Data is copied. arrival_wall_ns is the
 * wall-clock time the frame was received (used for adaptive jitter sizing). */
bool audio_buffer_push(struct audio_buffer *ab, const uint8_t *const *data,
		       const size_t *data_sizes, uint32_t frames,
		       uint32_t sample_rate, uint32_t channels, int format,
		       int speakers, int64_t pts_ns, int64_t arrival_wall_ns);

/* Try to pop the next audio frame. Returns false if buffer isn't ready.
 * The returned frame pointer is valid until the next pop/reset and is
 * unaffected by concurrent pushes. */
bool audio_buffer_pop(struct audio_buffer *ab, struct audio_buf_frame **out);

/* Record that the consumer wanted a frame but the buffer was empty. Grows the
 * adaptive cushion so playback rebuilds a deeper buffer after a stall. */
void audio_buffer_note_underrun(struct audio_buffer *ab);

/* Drop oldest frames until the level is back at the adaptive target. Used once
 * after the connect burst to discard the server's stale backlog and start near
 * live, instead of carrying that backlog as permanent latency. */
void audio_buffer_trim_to_target(struct audio_buffer *ab);

/* Get current buffer level in nanoseconds */
int64_t audio_buffer_level_ns(struct audio_buffer *ab);

/* Get the current adaptive target depth in nanoseconds */
int64_t audio_buffer_target_ns(struct audio_buffer *ab);

/* Get the current smoothed arrival-jitter estimate in nanoseconds */
int64_t audio_buffer_jitter_ns(struct audio_buffer *ab);

/* Check if buffer is primed and ready to output */
bool audio_buffer_is_ready(struct audio_buffer *ab);

/* Get buffer fill percentage (0.0 - 1.0) */
double audio_buffer_fill_ratio(struct audio_buffer *ab);

/* Read a consistent snapshot of all externally visible state. */
void audio_buffer_get_stats(struct audio_buffer *ab,
			    struct audio_buffer_stats *stats);
