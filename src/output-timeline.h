#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Keep timestamps far enough ahead that a 24/30/60 fps OBS video tick can
 * deliver each audio block before the mixer reaches it. Both audio and video
 * use this fixed lead, so it adds safety without changing lip sync. */
#define OUTPUT_TIMELINE_LEAD_NS     INT64_C(60000000)
#define OUTPUT_TIMELINE_MIN_LEAD_NS INT64_C(5000000)
#define OUTPUT_TIMELINE_MAX_LEAD_NS INT64_C(250000000)

struct output_timeline {
	int64_t next_ts;
	int64_t last_ts;
	uint64_t resync_count;
	bool started;
};

void output_timeline_reset(struct output_timeline *timeline);

/* Return the timestamp for one decoded audio block. Normal blocks remain
 * sample-contiguous. A discontinuity, late delivery, or corrupt duration
 * starts a fresh epoch safely ahead of the OBS mixer clock. */
int64_t output_timeline_next(struct output_timeline *timeline,
			     int64_t wall_now_ns, int64_t duration_ns,
			     bool discontinuity);
