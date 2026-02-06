#include "clock-tracker.h"
#include <string.h>
#include <math.h>

#define DEFAULT_WINDOW_NS  (2000000000LL)  /* 2 seconds */
#define DEFAULT_EMA_ALPHA  0.05

void clock_tracker_init(struct clock_tracker *ct)
{
	memset(ct, 0, sizeof(*ct));
	ct->stream_rate = 1.0;
	ct->smoothed_rate = 1.0;
	ct->window_ns = DEFAULT_WINDOW_NS;
	ct->ema_alpha = DEFAULT_EMA_ALPHA;
}

void clock_tracker_reset(struct clock_tracker *ct)
{
	int64_t window = ct->window_ns;
	double alpha = ct->ema_alpha;
	clock_tracker_init(ct);
	ct->window_ns = window;
	ct->ema_alpha = alpha;
}

void clock_tracker_record(struct clock_tracker *ct, int64_t stream_pts_ns,
			  int64_t wall_time_ns)
{
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

			/* Clamp to sane range */
			if (ct->stream_rate < 0.5)
				ct->stream_rate = 0.5;
			if (ct->stream_rate > 2.0)
				ct->stream_rate = 2.0;

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
}

double clock_tracker_get_rate(const struct clock_tracker *ct)
{
	return ct->stream_rate;
}

double clock_tracker_get_smoothed_rate(const struct clock_tracker *ct)
{
	return ct->smoothed_rate;
}

int64_t clock_tracker_get_drift(const struct clock_tracker *ct)
{
	return ct->drift_ns;
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
	if (rate < 0.5)
		rate = 0.5;
	if (rate > 2.0)
		rate = 2.0;

	int64_t adjusted_offset = (int64_t)((double)stream_offset / rate);
	return ct->anchor_wall_ns + adjusted_offset;
}
