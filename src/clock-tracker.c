#include "clock-tracker.h"
#include <string.h>
#include <math.h>

#define DEFAULT_WINDOW_NS  (5000000000LL)  /* 5 seconds */
/* Per-frame EMA factor. At ~47 updates/sec this gives a time constant of
 * ~1/(alpha*47) ≈ 2.7 s — slow enough to ride over the 1–4 s delivery
 * bursts that bonded cellular (SRTLA) produces without chasing them, while
 * still tracking genuine sustained clock drift. */
#define DEFAULT_EMA_ALPHA  0.008

void clock_tracker_init(struct clock_tracker *ct)
{
	memset(ct, 0, sizeof(*ct));
	ct->stream_rate = 1.0;
	ct->smoothed_rate = 1.0;
	ct->window_ns = DEFAULT_WINDOW_NS;
	ct->ema_alpha = DEFAULT_EMA_ALPHA;
	pthread_mutex_init(&ct->mutex, NULL);
}

void clock_tracker_free(struct clock_tracker *ct)
{
	pthread_mutex_destroy(&ct->mutex);
}

void clock_tracker_reset(struct clock_tracker *ct)
{
	pthread_mutex_lock(&ct->mutex);

	/* Reset measurement state but keep configuration and the mutex
	 * intact (do NOT memset — that would clobber the live mutex). */
	memset(ct->history, 0, sizeof(ct->history));
	ct->history_count = 0;
	ct->history_head = 0;
	ct->stream_rate = 1.0;
	ct->smoothed_rate = 1.0;
	ct->drift_ns = 0;
	ct->anchor_stream_ns = 0;
	ct->anchor_wall_ns = 0;
	ct->anchor_set = false;

	pthread_mutex_unlock(&ct->mutex);
}

void clock_tracker_record(struct clock_tracker *ct, int64_t stream_pts_ns,
			  int64_t wall_time_ns)
{
	pthread_mutex_lock(&ct->mutex);

	/* Set anchor on first sample */
	if (!ct->anchor_set) {
		ct->anchor_stream_ns = stream_pts_ns;
		ct->anchor_wall_ns = wall_time_ns;
		ct->anchor_set = true;
	}

	/* Store in circular buffer */
	int idx = ct->history_head % CLOCK_HISTORY_SIZE;
	ct->history[idx].stream_pts_ns = stream_pts_ns;
	ct->history[idx].wall_time_ns = wall_time_ns;
	ct->history_head++;
	if (ct->history_count < CLOCK_HISTORY_SIZE)
		ct->history_count++;

	/* Compute rate over the measurement window.
	 * Find the oldest sample within the window and compute
	 * stream_elapsed / wall_elapsed. */
	int oldest_idx = -1;
	int64_t oldest_wall = wall_time_ns;

	for (int i = 0; i < ct->history_count; i++) {
		int si = (ct->history_head - 1 - i + CLOCK_HISTORY_SIZE * 2) %
			 CLOCK_HISTORY_SIZE;
		int64_t sample_wall = ct->history[si].wall_time_ns;

		/* Only consider samples within the window */
		if (wall_time_ns - sample_wall > ct->window_ns)
			break;

		if (sample_wall < oldest_wall) {
			oldest_wall = sample_wall;
			oldest_idx = si;
		}
	}

	if (oldest_idx >= 0) {
		int64_t wall_elapsed = wall_time_ns -
				       ct->history[oldest_idx].wall_time_ns;
		int64_t stream_elapsed =
			stream_pts_ns -
			ct->history[oldest_idx].stream_pts_ns;

		if (wall_elapsed > 100000000LL) { /* need at least 100ms */
			ct->stream_rate =
				(double)stream_elapsed / (double)wall_elapsed;

			/* Clamp to sane range.  Real clock drift is
			 * never more than a fraction of a percent;
			 * anything beyond ±10 % is a transient burst
			 * from bursty network / decoder delivery. */
			if (ct->stream_rate < 0.90)
				ct->stream_rate = 0.90;
			if (ct->stream_rate > 1.10)
				ct->stream_rate = 1.10;

			/* Update EMA */
			ct->smoothed_rate =
				ct->ema_alpha * ct->stream_rate +
				(1.0 - ct->ema_alpha) * ct->smoothed_rate;
		}
	}

	/* Compute absolute drift */
	int64_t stream_elapsed = stream_pts_ns - ct->anchor_stream_ns;
	int64_t wall_elapsed = wall_time_ns - ct->anchor_wall_ns;
	ct->drift_ns = wall_elapsed - stream_elapsed;

	pthread_mutex_unlock(&ct->mutex);
}

double clock_tracker_get_rate(struct clock_tracker *ct)
{
	pthread_mutex_lock(&ct->mutex);
	double r = ct->stream_rate;
	pthread_mutex_unlock(&ct->mutex);
	return r;
}

double clock_tracker_get_smoothed_rate(struct clock_tracker *ct)
{
	pthread_mutex_lock(&ct->mutex);
	double r = ct->smoothed_rate;
	pthread_mutex_unlock(&ct->mutex);
	return r;
}

int64_t clock_tracker_get_drift(struct clock_tracker *ct)
{
	pthread_mutex_lock(&ct->mutex);
	int64_t d = ct->drift_ns;
	pthread_mutex_unlock(&ct->mutex);
	return d;
}

int64_t clock_tracker_adjust_timestamp(struct clock_tracker *ct,
				       int64_t stream_pts_ns)
{
	if (!ct->anchor_set)
		return stream_pts_ns;

	/* Map stream PTS to output time using the smoothed rate.
	 *
	 * If the stream runs at 0.98x realtime, we want our output
	 * timestamps to also advance at 0.98x, so OBS doesn't see
	 * gaps or bursts.
	 *
	 * output_time = anchor_wall + (stream_pts - anchor_stream) / smoothed_rate
	 *
	 * This stretches the stream timeline to match wall clock pace,
	 * effectively telling OBS "this audio should play at exactly
	 * the rate the stream is generating it."
	 */
	int64_t stream_offset = stream_pts_ns - ct->anchor_stream_ns;

	/* Scale by 1/rate: if stream is at 0.98x, we output at
	 * 1/0.98 = 1.02x wall time spacing, which means OBS will
	 * play it back slower to match the stream pace */
	double rate = ct->smoothed_rate;
	if (rate < 0.90)
		rate = 0.90;
	if (rate > 1.10)
		rate = 1.10;

	int64_t adjusted_offset = (int64_t)((double)stream_offset / rate);
	return ct->anchor_wall_ns + adjusted_offset;
}
