#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <util/threading.h>

/*
 * Clock Tracker — measures drift between stream PTS and wall clock.
 *
 * The core problem: OBS's built-in media source assumes the stream delivers
 * data at exactly 1.0x realtime. When the stream is slower (e.g. 0.98x),
 * the wall clock races ahead, causing audio frames to be output in bursts.
 *
 * This module measures the actual stream rate relative to wall clock and
 * provides a correction factor that the output logic can use to pace itself.
 */

/* Sized so the full measurement window fits in history. At ~47 audio
 * frames/sec a 5 s window needs ~235 samples; the old value of 64 capped
 * the window at ~1.35 s regardless of window_ns, making the rate estimate
 * noisy and biased toward the clamp under catch-up bursts. */
/*
 * Keep at least five seconds even for small 2.5 ms audio frames. The old
 * 320-sample ring silently shortened the configured window to 0.8 seconds
 * for such streams and made the rate estimate chase jitter.
 */
#define CLOCK_HISTORY_SIZE 2048

struct clock_sample {
	int64_t stream_pts_ns;  /* PTS from the stream (nanoseconds) */
	int64_t wall_time_ns;   /* wall clock at time of receipt */
};

struct clock_tracker {
	struct clock_sample history[CLOCK_HISTORY_SIZE];
	int history_count;
	int history_head;  /* circular buffer write position */

	/* Computed drift values */
	double stream_rate;       /* ratio: stream_elapsed / wall_elapsed (1.0 = perfect, <1.0 = slow) */
	int64_t drift_ns;         /* accumulated drift in nanoseconds (positive = stream behind wall) */

	/* Anchors: set when tracking starts */
	int64_t anchor_stream_ns;
	int64_t anchor_wall_ns;
	bool anchor_set;

	/* Smoothed rate for output pacing */
	double smoothed_rate;     /* EMA-filtered stream_rate */

	/* Configuration */
	int64_t window_ns;        /* measurement window size (default: 5 seconds) */
	double ema_alpha;         /* EMA smoothing factor (default: 0.008) */

	/* Guards cross-thread access: clock_tracker_record() runs on the
	 * media thread while the smoothed rate is read on the OBS tick
	 * thread. */
	pthread_mutex_t mutex;
};

/* Returns false if the internal mutex could not be created. */
bool clock_tracker_init(struct clock_tracker *ct);
void clock_tracker_free(struct clock_tracker *ct);
void clock_tracker_reset(struct clock_tracker *ct);

/* Record a stream PTS observation paired with wall time */
void clock_tracker_record(struct clock_tracker *ct, int64_t stream_pts_ns, int64_t wall_time_ns);

/* Get the current measured stream rate (< 1.0 means stream is slow) */
double clock_tracker_get_rate(struct clock_tracker *ct);

/* Get the smoothed rate suitable for output pacing */
double clock_tracker_get_smoothed_rate(struct clock_tracker *ct);

/* Get accumulated drift in nanoseconds */
int64_t clock_tracker_get_drift(struct clock_tracker *ct);

/* Given a stream PTS, compute the ideal wall-clock output time
 * accounting for measured drift. Returns adjusted timestamp in ns. */
int64_t clock_tracker_adjust_timestamp(struct clock_tracker *ct, int64_t stream_pts_ns);
