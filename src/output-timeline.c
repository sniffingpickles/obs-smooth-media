#include "output-timeline.h"

#include <limits.h>
#include <string.h>

static int64_t saturating_add(int64_t a, int64_t b)
{
	if (b > 0 && a > INT64_MAX - b)
		return INT64_MAX;
	if (b < 0 && a < INT64_MIN - b)
		return INT64_MIN;
	return a + b;
}

void output_timeline_reset(struct output_timeline *timeline)
{
	if (timeline)
		memset(timeline, 0, sizeof(*timeline));
}

int64_t output_timeline_next(struct output_timeline *timeline,
			     int64_t wall_now_ns, int64_t duration_ns,
			     bool discontinuity)
{
	if (!timeline)
		return 0;

	bool resync = !timeline->started || discontinuity || duration_ns <= 0;
	int64_t candidate = 0;
	if (!resync) {
		candidate = saturating_add(timeline->next_ts, duration_ns);
		int64_t minimum = saturating_add(
			wall_now_ns, OUTPUT_TIMELINE_MIN_LEAD_NS);
		int64_t maximum = saturating_add(
			wall_now_ns, OUTPUT_TIMELINE_MAX_LEAD_NS);
		resync = candidate < minimum || candidate > maximum;
	}

	if (resync) {
		candidate = saturating_add(wall_now_ns,
					   OUTPUT_TIMELINE_LEAD_NS);
		if (timeline->started)
			timeline->resync_count++;
	}

	if (timeline->started && candidate <= timeline->last_ts)
		candidate = timeline->last_ts == INT64_MAX
				    ? INT64_MAX
				    : timeline->last_ts + 1;

	timeline->started = true;
	timeline->next_ts = candidate;
	timeline->last_ts = candidate;
	return candidate;
}
