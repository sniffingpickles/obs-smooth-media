#include "audio-buffer.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MIN_BUFFER_NS  (80000000LL)   /* 80ms floor */
#define DEFAULT_MAX_BUFFER_NS  (500000000LL)  /* 500ms initial ceiling */

/* Adaptive-target tuning (all self-managed; no user-facing knobs). */
#define TARGET_JITTER_COEF       3              /* target = floor + 3·jitter */
#define TARGET_CAP_NS            (750000000LL)  /* extreme-link target cap */
#define MAX_HARD_CAP_NS          (1200000000LL) /* absolute 1.2s ceiling */
#define MAX_MARGIN_NS            (250000000LL)  /* drop at target + 250ms */
#define DELIVERY_GAP_MARGIN_NS   (40000000LL)   /* scheduling headroom */
#define DELIVERY_GAP_DECAY_DIV   20             /* peak decays 50ms/s */
#define UNDERRUN_CREDIT_STEP_NS  (60000000LL)   /* +60ms per starvation */
#define UNDERRUN_CREDIT_CAP_NS   (400000000LL)  /* credit cap */
#define UNDERRUN_CREDIT_DECAY_NS (500000LL)     /* ~23ms/s at AAC cadence */
/* Retain ordinary codec-frame allocations for reuse, but release unusually
 * large slots. Otherwise a hostile sequence can rotate a multi-megabyte frame
 * through every ring slot and leave gigabytes resident after it is dropped. */
#define RETAINED_FRAME_CAPACITY_MAX (64U * 1024U)

static void free_frame_data(struct audio_buf_frame *f)
{
	for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++) {
		if (f->data[i]) {
			free(f->data[i]);
			f->data[i] = NULL;
		}
		f->data_size[i] = 0;
		f->data_capacity[i] = 0;
	}
	f->valid = false;
}

static void clear_frame_metadata(struct audio_buf_frame *f)
{
	for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++)
		f->data_size[i] = 0;
	f->frames = 0;
	f->sample_rate = 0;
	f->channels = 0;
	f->pts_ns = 0;
	f->format = 0;
	f->speakers = 0;
	f->valid = false;
}

static bool frame_storage_is_large(const struct audio_buf_frame *f)
{
	size_t total = 0;
	for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++) {
		if (f->data_capacity[i] > RETAINED_FRAME_CAPACITY_MAX - total)
			return true;
		total += f->data_capacity[i];
	}
	return false;
}

static void clear_frame_for_reuse(struct audio_buf_frame *f)
{
	if (frame_storage_is_large(f))
		free_frame_data(f);
	clear_frame_metadata(f);
}

bool audio_buffer_init(struct audio_buffer *ab)
{
	if (!ab)
		return false;
	memset(ab, 0, sizeof(*ab));
	if (pthread_mutex_init(&ab->mutex, NULL) != 0)
		return false;
	ab->min_buffer_ns = DEFAULT_MIN_BUFFER_NS;
	ab->max_buffer_ns = DEFAULT_MAX_BUFFER_NS;
	ab->target_buffer_ns = DEFAULT_MIN_BUFFER_NS;
	ab->last_output_pts = -1;
	return true;
}

/* Recompute the adaptive target from measured jitter + underrun credit, and
 * derive the drop ceiling from it. Caller must hold the mutex. */
static void recalc_target(struct audio_buffer *ab)
{
	int64_t target = ab->min_buffer_ns;
	if (ab->jitter_ns > (TARGET_CAP_NS - target) / TARGET_JITTER_COEF) {
		target = TARGET_CAP_NS;
	} else {
		target += TARGET_JITTER_COEF * ab->jitter_ns;
	}
	if (ab->underrun_credit_ns > TARGET_CAP_NS - target)
		target = TARGET_CAP_NS;
	else
		target += ab->underrun_credit_ns;
	if (ab->delivery_gap_ns > 0) {
		int64_t gap_target = ab->delivery_gap_ns;
		if (gap_target > TARGET_CAP_NS - DELIVERY_GAP_MARGIN_NS)
			gap_target = TARGET_CAP_NS;
		else
			gap_target += DELIVERY_GAP_MARGIN_NS;
		if (target < gap_target)
			target = gap_target;
	}
	ab->target_buffer_ns = target;

	int64_t cap = target + MAX_MARGIN_NS;
	if (cap > MAX_HARD_CAP_NS)
		cap = MAX_HARD_CAP_NS;
	ab->max_buffer_ns = cap;
}

void audio_buffer_free(struct audio_buffer *ab)
{
	if (!ab)
		return;

	pthread_mutex_lock(&ab->mutex);
	for (int i = 0; i < AUDIO_BUF_MAX_FRAMES; i++)
		free_frame_data(&ab->frames[i]);
	free_frame_data(&ab->out_frame);
	pthread_mutex_unlock(&ab->mutex);
	pthread_mutex_destroy(&ab->mutex);
	memset(ab, 0, sizeof(*ab));
}

void audio_buffer_reset(struct audio_buffer *ab)
{
	if (!ab)
		return;

	pthread_mutex_lock(&ab->mutex);

	for (int i = 0; i < AUDIO_BUF_MAX_FRAMES; i++)
		free_frame_data(&ab->frames[i]);
	free_frame_data(&ab->out_frame);

	ab->read_pos = 0;
	ab->write_pos = 0;
	ab->count = 0;
	ab->max_buffer_ns = DEFAULT_MAX_BUFFER_NS;
	ab->target_buffer_ns = ab->min_buffer_ns;
	ab->jitter_ns = 0;
	ab->underrun_credit_ns = 0;
	ab->prev_arrival_ns = 0;
	ab->prev_arrival_pts_ns = 0;
	ab->have_arrival_ref = false;
	ab->delivery_gap_ns = 0;
	ab->last_gap_decay_ns = 0;
	ab->primed = false;
	ab->total_buffered_ns = 0;
	ab->last_output_pts = -1;
	ab->frames_in = 0;
	ab->frames_out = 0;
	ab->frames_dropped = 0;

	pthread_mutex_unlock(&ab->mutex);
}

void audio_buffer_set_minimum(struct audio_buffer *ab, int64_t minimum_ns)
{
	if (!ab)
		return;
	if (minimum_ns < 0)
		minimum_ns = 0;
	if (minimum_ns > TARGET_CAP_NS)
		minimum_ns = TARGET_CAP_NS;

	pthread_mutex_lock(&ab->mutex);
	ab->min_buffer_ns = minimum_ns;
	recalc_target(ab);
	pthread_mutex_unlock(&ab->mutex);
}

static int64_t frame_duration_ns(uint32_t samples, uint32_t sample_rate)
{
	if (sample_rate == 0)
		return 0;
	return (int64_t)samples * 1000000000LL / (int64_t)sample_rate;
}

static void drop_oldest(struct audio_buffer *ab)
{
	if (ab->count <= 0)
		return;

	struct audio_buf_frame *f = &ab->frames[ab->read_pos];
	ab->total_buffered_ns -= frame_duration_ns(f->frames, f->sample_rate);
	if (ab->total_buffered_ns < 0)
		ab->total_buffered_ns = 0;
	clear_frame_for_reuse(f);
	ab->read_pos = (ab->read_pos + 1) % AUDIO_BUF_MAX_FRAMES;
	ab->count--;
	ab->frames_dropped++;
}

static bool valid_push(const uint8_t *const *data, const size_t *data_sizes,
		       uint32_t frames, uint32_t sample_rate, uint32_t channels)
{
	if (!data || !data_sizes || frames == 0 || sample_rate == 0 ||
	    channels == 0 || channels > AUDIO_BUF_MAX_PLANES)
		return false;

	int64_t duration = frame_duration_ns(frames, sample_rate);
	if (duration <= 0 || duration > MAX_HARD_CAP_NS)
		return false;

	size_t total = 0;
	bool have_data = false;
	for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++) {
		if ((data[i] == NULL) != (data_sizes[i] == 0))
			return false;
		if (data_sizes[i] > AUDIO_BUF_MAX_FRAME_BYTES - total)
			return false;
		total += data_sizes[i];
		have_data = have_data || data_sizes[i] != 0;
	}
	return have_data;
}

bool audio_buffer_push(struct audio_buffer *ab, const uint8_t *const *data,
		       const size_t *data_sizes, uint32_t frames,
		       uint32_t sample_rate, uint32_t channels, int format,
		       int speakers, int64_t pts_ns, int64_t arrival_wall_ns)
{
	if (!ab || !valid_push(data, data_sizes, frames, sample_rate, channels))
		return false;

	pthread_mutex_lock(&ab->mutex);

	/* ── Adaptive jitter estimation (RFC 3550 interarrival jitter) ──
	 * D = (arrival_n - arrival_{n-1}) - (pts_n - pts_{n-1}): how much the
	 * delivery gap deviated from the media gap. J += (|D| - J)/16. */
	if (ab->have_arrival_ref && arrival_wall_ns >= ab->prev_arrival_ns &&
	    pts_ns >= ab->prev_arrival_pts_ns) {
		uint64_t arrival_delta = (uint64_t)arrival_wall_ns -
					 (uint64_t)ab->prev_arrival_ns;
		uint64_t pts_delta =
			(uint64_t)pts_ns - (uint64_t)ab->prev_arrival_pts_ns;
		uint64_t diff = arrival_delta > pts_delta
					? arrival_delta - pts_delta
					: pts_delta - arrival_delta;
		int64_t d =
			diff > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)diff;
		ab->jitter_ns += (d - ab->jitter_ns) / 16;
		if (ab->jitter_ns < 0)
			ab->jitter_ns = 0;

		/* Preserve the recent peak delivery gap. RFC-style jitter is an
		 * average and underestimates protocols that release a large group
		 * every few hundred milliseconds: the many zero-gap frames in the
		 * group immediately wash out the one important gap. */
		if (ab->last_gap_decay_ns != 0 &&
		    arrival_wall_ns > ab->last_gap_decay_ns) {
			int64_t elapsed =
				arrival_wall_ns - ab->last_gap_decay_ns;
			int64_t decay = elapsed / DELIVERY_GAP_DECAY_DIV;
			ab->delivery_gap_ns =
				decay >= ab->delivery_gap_ns
					? 0
					: ab->delivery_gap_ns - decay;
		}
		ab->last_gap_decay_ns = arrival_wall_ns;
		int64_t arrival_delta_ns = arrival_delta > (uint64_t)INT64_MAX
						   ? INT64_MAX
						   : (int64_t)arrival_delta;
		if (arrival_delta_ns > ab->delivery_gap_ns)
			ab->delivery_gap_ns = arrival_delta_ns;
	}
	ab->prev_arrival_ns = arrival_wall_ns;
	ab->prev_arrival_pts_ns = pts_ns;
	ab->have_arrival_ref = true;

	/* Slowly bleed off the post-underrun credit while the link is calm. */
	if (ab->underrun_credit_ns > 0) {
		ab->underrun_credit_ns -= UNDERRUN_CREDIT_DECAY_NS;
		if (ab->underrun_credit_ns < 0)
			ab->underrun_credit_ns = 0;
	}

	recalc_target(ab);

	struct audio_buf_frame *f = &ab->frames[ab->write_pos];
	bool release_on_full_drop =
		ab->count >= AUDIO_BUF_MAX_FRAMES && frame_storage_is_large(f);

	/* Allocate every required growth before modifying the queue or the slot.
	 * A failed allocation therefore leaves both the old frame and all ring
	 * indices intact. */
	uint8_t *grown[AUDIO_BUF_MAX_PLANES] = {0};
	for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++) {
		if (data_sizes[i] > 0 &&
		    (release_on_full_drop ||
		     data_sizes[i] > f->data_capacity[i])) {
			grown[i] = malloc(data_sizes[i]);
			if (!grown[i]) {
				for (int j = 0; j < AUDIO_BUF_MAX_PLANES; j++)
					free(grown[j]);
				pthread_mutex_unlock(&ab->mutex);
				return false;
			}
		}
	}

	/* If the ring is full, write_pos is the oldest slot. Drop it only after
	 * all allocations above have succeeded. */
	if (ab->count >= AUDIO_BUF_MAX_FRAMES)
		drop_oldest(ab);

	for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++) {
		if (grown[i]) {
			free(f->data[i]);
			f->data[i] = grown[i];
			f->data_capacity[i] = data_sizes[i];
		}
		if (data_sizes[i] > 0) {
			memcpy(f->data[i], data[i], data_sizes[i]);
			f->data_size[i] = data_sizes[i];
		} else {
			f->data_size[i] = 0;
		}
	}

	f->frames = frames;
	f->sample_rate = sample_rate;
	f->channels = channels;
	f->pts_ns = pts_ns;
	f->format = format;
	f->speakers = speakers;
	f->valid = true;

	ab->write_pos = (ab->write_pos + 1) % AUDIO_BUF_MAX_FRAMES;
	ab->count++;
	ab->frames_in++;

	/* Update buffered duration */
	ab->total_buffered_ns += frame_duration_ns(frames, sample_rate);

	/* Enforce the latency ceiling after adding the new frame. Keep at least
	 * the newest frame so a legal but unusually large decoder frame cannot
	 * turn the buffer into a permanent empty/underrun loop. */
	while (ab->count > 1 && ab->total_buffered_ns > ab->max_buffer_ns)
		drop_oldest(ab);

	/* Check if primed — against the adaptive target, not a fixed floor */
	if (!ab->primed && ab->total_buffered_ns >= ab->target_buffer_ns)
		ab->primed = true;

	pthread_mutex_unlock(&ab->mutex);
	return true;
}

void audio_buffer_note_underrun(struct audio_buffer *ab)
{
	if (!ab)
		return;
	pthread_mutex_lock(&ab->mutex);
	/* An empty primed queue is a discontinuity, not ordinary low fill. Hold
	 * newly arriving frames until the larger recovery target is available.
	 * Otherwise one frame is released and the queue empties again, producing
	 * a repeating audio/silence cycle and a stream of late timestamps. */
	ab->primed = false;
	ab->underrun_credit_ns += UNDERRUN_CREDIT_STEP_NS;
	if (ab->underrun_credit_ns > UNDERRUN_CREDIT_CAP_NS)
		ab->underrun_credit_ns = UNDERRUN_CREDIT_CAP_NS;
	recalc_target(ab);
	pthread_mutex_unlock(&ab->mutex);
}

void audio_buffer_note_discontinuity(struct audio_buffer *ab)
{
	if (!ab)
		return;
	pthread_mutex_lock(&ab->mutex);
	ab->prev_arrival_ns = 0;
	ab->prev_arrival_pts_ns = 0;
	ab->have_arrival_ref = false;
	ab->delivery_gap_ns = 0;
	ab->last_gap_decay_ns = 0;
	ab->jitter_ns = 0;
	recalc_target(ab);
	pthread_mutex_unlock(&ab->mutex);
}

void audio_buffer_trim_to_target(struct audio_buffer *ab)
{
	if (!ab)
		return;
	pthread_mutex_lock(&ab->mutex);
	while (ab->count > 1) {
		struct audio_buf_frame *oldest = &ab->frames[ab->read_pos];
		int64_t after_drop =
			ab->total_buffered_ns -
			frame_duration_ns(oldest->frames, oldest->sample_rate);
		if (after_drop < ab->target_buffer_ns)
			break;
		drop_oldest(ab);
	}
	if (ab->total_buffered_ns < 0)
		ab->total_buffered_ns = 0;
	pthread_mutex_unlock(&ab->mutex);
}

bool audio_buffer_pop(struct audio_buffer *ab, struct audio_buf_frame **out)
{
	if (!out)
		return false;
	*out = NULL;
	if (!ab)
		return false;

	pthread_mutex_lock(&ab->mutex);

	if (ab->count == 0) {
		pthread_mutex_unlock(&ab->mutex);
		return false;
	}

	/* Don't output until primed */
	if (!ab->primed) {
		pthread_mutex_unlock(&ab->mutex);
		return false;
	}

	struct audio_buf_frame *f = &ab->frames[ab->read_pos];
	if (!f->valid) {
		pthread_mutex_unlock(&ab->mutex);
		return false;
	}

	/* Swap the popped slot with the staging frame. This transfers ownership
	 * without allocation or copying and keeps the returned storage isolated
	 * from concurrent pushes until the next pop/reset. */
	struct audio_buf_frame *o = &ab->out_frame;
	struct audio_buf_frame tmp = *o;
	*o = *f;
	*f = tmp;
	clear_frame_for_reuse(f);

	/* The popped frame now lives in out_frame; f is the cleared staging
	 * storage. Account against o or the reported level never decreases. */
	ab->last_output_pts = o->pts_ns;
	ab->total_buffered_ns -= frame_duration_ns(o->frames, o->sample_rate);
	if (ab->total_buffered_ns < 0)
		ab->total_buffered_ns = 0;

	ab->read_pos = (ab->read_pos + 1) % AUDIO_BUF_MAX_FRAMES;
	ab->count--;
	ab->frames_out++;

	/* Staying primed during normal drain avoids needless pauses. A genuine
	 * empty-queue underrun explicitly clears primed in
	 * audio_buffer_note_underrun(), after the consumer has observed it. */

	*out = o;
	pthread_mutex_unlock(&ab->mutex);
	return true;
}

void audio_buffer_get_stats(struct audio_buffer *ab,
			    struct audio_buffer_stats *stats)
{
	if (!stats)
		return;
	memset(stats, 0, sizeof(*stats));
	if (!ab)
		return;

	pthread_mutex_lock(&ab->mutex);
	stats->count = ab->count;
	stats->primed = ab->primed;
	stats->level_ns = ab->total_buffered_ns;
	stats->target_ns = ab->target_buffer_ns;
	stats->max_ns = ab->max_buffer_ns;
	stats->jitter_ns = ab->jitter_ns;
	stats->delivery_gap_ns = ab->delivery_gap_ns;
	stats->frames_in = ab->frames_in;
	stats->frames_out = ab->frames_out;
	stats->frames_dropped = ab->frames_dropped;
	pthread_mutex_unlock(&ab->mutex);
}

int64_t audio_buffer_level_ns(struct audio_buffer *ab)
{
	struct audio_buffer_stats stats;
	audio_buffer_get_stats(ab, &stats);
	return stats.level_ns;
}

int64_t audio_buffer_target_ns(struct audio_buffer *ab)
{
	struct audio_buffer_stats stats;
	audio_buffer_get_stats(ab, &stats);
	return stats.target_ns;
}

int64_t audio_buffer_jitter_ns(struct audio_buffer *ab)
{
	struct audio_buffer_stats stats;
	audio_buffer_get_stats(ab, &stats);
	return stats.jitter_ns;
}

bool audio_buffer_is_ready(struct audio_buffer *ab)
{
	struct audio_buffer_stats stats;
	audio_buffer_get_stats(ab, &stats);
	return stats.primed;
}

double audio_buffer_fill_ratio(struct audio_buffer *ab)
{
	struct audio_buffer_stats stats;
	audio_buffer_get_stats(ab, &stats);
	if (stats.max_ns <= 0)
		return 0.0;
	double ratio = (double)stats.level_ns / (double)stats.max_ns;
	if (ratio > 1.0)
		ratio = 1.0;
	return ratio;
}
