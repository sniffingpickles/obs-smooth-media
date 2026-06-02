#include "audio-buffer.h"
#include <stdlib.h>
#include <string.h>

#define DEFAULT_MIN_BUFFER_NS  (80000000LL)   /* 80ms floor */
#define DEFAULT_MAX_BUFFER_NS  (500000000LL)  /* 500ms ceiling (start value) */

/* Adaptive-target tuning (all self-managed; no user-facing knobs). */
#define TARGET_JITTER_COEF     3              /* target = floor + 3·jitter */
#define TARGET_CAP_NS          (600000000LL)  /* never aim above 600ms latency */
#define MAX_HARD_CAP_NS        (1200000000LL) /* absolute latency ceiling 1.2s */
#define MAX_MARGIN_NS          (250000000LL)  /* drop level = target + 250ms */
#define UNDERRUN_CREDIT_STEP_NS (60000000LL)  /* +60ms cushion per underrun */
#define UNDERRUN_CREDIT_CAP_NS  (400000000LL) /* credit caps at +400ms */
#define UNDERRUN_CREDIT_DECAY_NS (500000LL)   /* decay ~0.5ms/frame (~23ms/s) */

static void free_frame_data(struct audio_buf_frame *f)
{
	for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++) {
		if (f->data[i]) {
			free(f->data[i]);
			f->data[i] = NULL;
		}
		f->data_size[i] = 0;
	}
	f->valid = false;
}

void audio_buffer_init(struct audio_buffer *ab)
{
	memset(ab, 0, sizeof(*ab));
	pthread_mutex_init(&ab->mutex, NULL);
	ab->min_buffer_ns = DEFAULT_MIN_BUFFER_NS;
	ab->max_buffer_ns = DEFAULT_MAX_BUFFER_NS;
	ab->target_buffer_ns = DEFAULT_MIN_BUFFER_NS;
	ab->last_output_pts = -1;
}

/* Recompute the adaptive target from measured jitter + underrun credit, and
 * derive the drop ceiling from it. Caller must hold the mutex. */
static void recalc_target(struct audio_buffer *ab)
{
	int64_t target = ab->min_buffer_ns +
			 TARGET_JITTER_COEF * ab->jitter_ns +
			 ab->underrun_credit_ns;
	if (target < ab->min_buffer_ns)
		target = ab->min_buffer_ns;
	if (target > TARGET_CAP_NS)
		target = TARGET_CAP_NS;
	ab->target_buffer_ns = target;

	int64_t cap = target + MAX_MARGIN_NS;
	if (cap > MAX_HARD_CAP_NS)
		cap = MAX_HARD_CAP_NS;
	ab->max_buffer_ns = cap;
}

void audio_buffer_free(struct audio_buffer *ab)
{
	pthread_mutex_destroy(&ab->mutex);
	for (int i = 0; i < AUDIO_BUF_MAX_FRAMES; i++)
		free_frame_data(&ab->frames[i]);
	free_frame_data(&ab->out_frame);
	memset(ab, 0, sizeof(*ab));
}

void audio_buffer_reset(struct audio_buffer *ab)
{
	int64_t min_ns = ab->min_buffer_ns;
	audio_buffer_free(ab);
	audio_buffer_init(ab);
	/* Preserve the configured floor; clear all adaptive state so a fresh
	 * connection re-learns the link's jitter from scratch. */
	ab->min_buffer_ns = min_ns;
	ab->target_buffer_ns = min_ns;
}

static int64_t frame_duration_ns(uint32_t samples, uint32_t sample_rate)
{
	if (sample_rate == 0)
		return 0;
	return (int64_t)samples * 1000000000LL / (int64_t)sample_rate;
}

static void recalc_buffered(struct audio_buffer *ab)
{
	int64_t total = 0;
	for (int i = 0; i < ab->count; i++) {
		int idx = (ab->read_pos + i) % AUDIO_BUF_MAX_FRAMES;
		struct audio_buf_frame *f = &ab->frames[idx];
		if (f->valid)
			total += frame_duration_ns(f->frames, f->sample_rate);
	}
	ab->total_buffered_ns = total;
}

bool audio_buffer_push(struct audio_buffer *ab, const uint8_t *const *data,
		       const size_t *data_sizes, uint32_t frames,
		       uint32_t sample_rate, uint32_t channels,
		       int format, int speakers, int64_t pts_ns,
		       int64_t arrival_wall_ns)
{
	pthread_mutex_lock(&ab->mutex);

	/* ── Adaptive jitter estimation (RFC 3550 interarrival jitter) ──
	 * D = (arrival_n - arrival_{n-1}) - (pts_n - pts_{n-1}): how much the
	 * delivery gap deviated from the media gap. J += (|D| - J)/16. */
	if (ab->have_arrival_ref) {
		int64_t d = (arrival_wall_ns - ab->prev_arrival_ns) -
			    (pts_ns - ab->prev_arrival_pts_ns);
		if (d < 0)
			d = -d;
		ab->jitter_ns += (d - ab->jitter_ns) / 16;
		if (ab->jitter_ns < 0)
			ab->jitter_ns = 0;
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

	/* If buffer is full, drop oldest frame */
	if (ab->count >= AUDIO_BUF_MAX_FRAMES) {
		free_frame_data(&ab->frames[ab->read_pos]);
		ab->read_pos = (ab->read_pos + 1) % AUDIO_BUF_MAX_FRAMES;
		ab->count--;
		ab->frames_dropped++;
	}

	/* Also enforce max buffer duration — drop oldest if too much latency */
	while (ab->count > 0 && ab->total_buffered_ns > ab->max_buffer_ns) {
		free_frame_data(&ab->frames[ab->read_pos]);
		ab->read_pos = (ab->read_pos + 1) % AUDIO_BUF_MAX_FRAMES;
		ab->count--;
		ab->frames_dropped++;
		recalc_buffered(ab);
	}

	struct audio_buf_frame *f = &ab->frames[ab->write_pos];

	/* Copy audio data */
	int num_planes = 0;
	for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++) {
		if (data[i] && data_sizes[i] > 0) {
			/* Realloc if needed */
			if (f->data_size[i] < data_sizes[i]) {
				free(f->data[i]);
				f->data[i] = malloc(data_sizes[i]);
				if (!f->data[i]) {
					f->data_size[i] = 0;
					pthread_mutex_unlock(&ab->mutex);
					return false;
				}
			}
			memcpy(f->data[i], data[i], data_sizes[i]);
			f->data_size[i] = data_sizes[i];
			num_planes++;
		} else {
			/* Clear unused planes */
			if (f->data[i]) {
				free(f->data[i]);
				f->data[i] = NULL;
			}
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

	/* Check if primed — against the adaptive target, not a fixed floor */
	if (!ab->primed && ab->total_buffered_ns >= ab->target_buffer_ns)
		ab->primed = true;

	pthread_mutex_unlock(&ab->mutex);
	return true;
}

void audio_buffer_note_underrun(struct audio_buffer *ab)
{
	pthread_mutex_lock(&ab->mutex);
	ab->underrun_credit_ns += UNDERRUN_CREDIT_STEP_NS;
	if (ab->underrun_credit_ns > UNDERRUN_CREDIT_CAP_NS)
		ab->underrun_credit_ns = UNDERRUN_CREDIT_CAP_NS;
	recalc_target(ab);
	pthread_mutex_unlock(&ab->mutex);
}

bool audio_buffer_pop(struct audio_buffer *ab, struct audio_buf_frame **out)
{
	*out = NULL;

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

	/* Copy the popped frame into the staging buffer while we hold the
	 * lock. The ring slot can be reused by a concurrent push the moment
	 * we unlock, so the consumer must operate on its own copy. */
	struct audio_buf_frame *o = &ab->out_frame;
	for (int i = 0; i < AUDIO_BUF_MAX_PLANES; i++) {
		if (f->data[i] && f->data_size[i] > 0) {
			if (o->data_size[i] < f->data_size[i]) {
				free(o->data[i]);
				o->data[i] = malloc(f->data_size[i]);
				if (!o->data[i]) {
					o->data_size[i] = 0;
					pthread_mutex_unlock(&ab->mutex);
					return false;
				}
			}
			memcpy(o->data[i], f->data[i], f->data_size[i]);
			o->data_size[i] = f->data_size[i];
		} else {
			if (o->data[i]) {
				free(o->data[i]);
				o->data[i] = NULL;
			}
			o->data_size[i] = 0;
		}
	}
	o->frames = f->frames;
	o->sample_rate = f->sample_rate;
	o->channels = f->channels;
	o->pts_ns = f->pts_ns;
	o->format = f->format;
	o->speakers = f->speakers;
	o->valid = true;

	ab->last_output_pts = f->pts_ns;
	ab->total_buffered_ns -= frame_duration_ns(f->frames, f->sample_rate);
	if (ab->total_buffered_ns < 0)
		ab->total_buffered_ns = 0;

	ab->read_pos = (ab->read_pos + 1) % AUDIO_BUF_MAX_FRAMES;
	ab->count--;
	ab->frames_out++;

	/* Once primed, stay primed. Only audio_buffer_reset() clears this.
	 * Un-priming on empty caused a stutter cycle: fill 500ms → burst
	 * drain → silence while re-filling → repeat every 500ms. */

	*out = o;
	pthread_mutex_unlock(&ab->mutex);
	return true;
}

int64_t audio_buffer_level_ns(const struct audio_buffer *ab)
{
	return ab->total_buffered_ns;
}

int64_t audio_buffer_target_ns(const struct audio_buffer *ab)
{
	return ab->target_buffer_ns;
}

bool audio_buffer_is_ready(const struct audio_buffer *ab)
{
	return ab->primed;
}

double audio_buffer_fill_ratio(const struct audio_buffer *ab)
{
	if (ab->max_buffer_ns <= 0)
		return 0.0;
	double ratio = (double)ab->total_buffered_ns / (double)ab->max_buffer_ns;
	if (ratio > 1.0)
		ratio = 1.0;
	return ratio;
}
