#include "audio-buffer.h"
#include "clock-tracker.h"
#include "output-timeline.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NS_PER_SEC INT64_C(1000000000)
#define NS_PER_MS INT64_C(1000000)

static int failures;

#define CHECK(condition)                                                       \
	do {                                                                   \
		if (!(condition)) {                                            \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, \
				__LINE__, #condition);                         \
			failures++;                                            \
		}                                                              \
	} while (0)

static bool push_mono(struct audio_buffer *buffer, uint64_t value,
		      uint32_t frames, int64_t pts, int64_t arrival)
{
	const uint8_t *planes[AUDIO_BUF_MAX_PLANES] = {0};
	size_t sizes[AUDIO_BUF_MAX_PLANES] = {0};
	planes[0] = (const uint8_t *)&value;
	sizes[0] = sizeof(value);
	return audio_buffer_push(buffer, planes, sizes, frames, 48000, 1, 1, 1,
				 pts, arrival);
}

static void test_buffer_basic(void)
{
	struct audio_buffer buffer;
	CHECK(audio_buffer_init(&buffer));

	for (uint64_t i = 0; i < 4; i++) {
		CHECK(push_mono(&buffer, i, 960, (int64_t)i * 20 * NS_PER_MS,
				(int64_t)i * 20 * NS_PER_MS));
		CHECK(audio_buffer_is_ready(&buffer) == (i == 3));
	}

	struct audio_buffer_stats stats;
	audio_buffer_get_stats(&buffer, &stats);
	CHECK(stats.count == 4);
	CHECK(stats.level_ns == 80 * NS_PER_MS);
	CHECK(stats.frames_in == 4);

	struct audio_buf_frame *out = NULL;
	CHECK(audio_buffer_pop(&buffer, &out));
	CHECK(out != NULL);
	CHECK(out->pts_ns == 0);
	CHECK(out->data_size[0] == sizeof(uint64_t));
	uint64_t first = UINT64_MAX;
	memcpy(&first, out->data[0], sizeof(first));
	CHECK(first == 0);
	audio_buffer_get_stats(&buffer, &stats);
	CHECK(stats.count == 3);
	CHECK(stats.level_ns == 60 * NS_PER_MS);

	/* A producer push must not overwrite the staging frame returned to the
	 * consumer. It remains valid until the next pop/reset. */
	CHECK(push_mono(&buffer, 99, 960, 99 * NS_PER_MS, 99 * NS_PER_MS));
	memcpy(&first, out->data[0], sizeof(first));
	CHECK(first == 0);

	audio_buffer_free(&buffer);
}

static void test_buffer_trim_and_adaptation(void)
{
	struct audio_buffer buffer;
	CHECK(audio_buffer_init(&buffer));

	for (uint64_t i = 0; i < 30; i++)
		CHECK(push_mono(&buffer, i, 960, (int64_t)i * 20 * NS_PER_MS,
				(int64_t)i * 20 * NS_PER_MS));

	audio_buffer_trim_to_target(&buffer);
	struct audio_buffer_stats stats;
	audio_buffer_get_stats(&buffer, &stats);
	CHECK(stats.level_ns >= stats.target_ns);
	CHECK(stats.level_ns < stats.target_ns + 20 * NS_PER_MS);
	CHECK(stats.frames_dropped > 0);

	for (int i = 0; i < 7; i++)
		audio_buffer_note_underrun(&buffer);
	audio_buffer_get_stats(&buffer, &stats);
	CHECK(stats.target_ns == 480 * NS_PER_MS);

	audio_buffer_reset(&buffer);
	for (uint64_t i = 0; i < 200; i++) {
		/* 120-sample (2.5 ms) frames could never prime a 480+ ms
		 * target in the old 128-slot ring. */
		if (i < 7)
			audio_buffer_note_underrun(&buffer);
		CHECK(push_mono(&buffer, i, 120, (int64_t)i * 2500000,
				(int64_t)i * 2500000));
	}
	audio_buffer_get_stats(&buffer, &stats);
	CHECK(stats.target_ns >= 300 * NS_PER_MS);
	CHECK(stats.primed);
	CHECK(stats.count > 128);

	audio_buffer_free(&buffer);
}

static void test_buffer_reprime_after_underrun(void)
{
	struct audio_buffer buffer;
	CHECK(audio_buffer_init(&buffer));

	for (uint64_t i = 0; i < 4; i++)
		CHECK(push_mono(&buffer, i, 960, (int64_t)i * 20 * NS_PER_MS,
				(int64_t)i * 20 * NS_PER_MS));
	CHECK(audio_buffer_is_ready(&buffer));

	struct audio_buf_frame *out = NULL;
	for (int i = 0; i < 4; i++)
		CHECK(audio_buffer_pop(&buffer, &out));
	CHECK(!audio_buffer_pop(&buffer, &out));

	audio_buffer_note_underrun(&buffer);
	struct audio_buffer_stats stats;
	audio_buffer_get_stats(&buffer, &stats);
	CHECK(!stats.primed);
	CHECK(stats.target_ns == 140 * NS_PER_MS);

	/* Recovery must wait for the new target instead of releasing isolated
	 * frames as they arrive. */
	uint64_t i = 0;
	for (; i < 64 && !audio_buffer_is_ready(&buffer); i++) {
		CHECK(push_mono(&buffer, 10 + i, 960,
				(10 + (int64_t)i) * 20 * NS_PER_MS,
				(10 + (int64_t)i) * 20 * NS_PER_MS));
		if (!audio_buffer_is_ready(&buffer))
			CHECK(!audio_buffer_pop(&buffer, &out));
	}
	CHECK(i < 64);
	CHECK(audio_buffer_is_ready(&buffer));
	CHECK(audio_buffer_pop(&buffer, &out));

	audio_buffer_free(&buffer);
}

static void test_output_timeline_recovery(void)
{
	struct output_timeline timeline;
	output_timeline_reset(&timeline);

	const int64_t frame = 20 * NS_PER_MS;
	int64_t wall = 5 * NS_PER_SEC;
	int64_t first = output_timeline_next(&timeline, wall, frame, false);
	CHECK(first == wall + OUTPUT_TIMELINE_LEAD_NS);

	/* Ordinary output stays sample-contiguous and safely ahead of the OBS
	 * mixer clock. */
	int64_t second = output_timeline_next(
		&timeline, wall + 33 * NS_PER_MS, frame, false);
	CHECK(second == first + frame);
	CHECK(second >= wall + 33 * NS_PER_MS +
			       OUTPUT_TIMELINE_MIN_LEAD_NS);

	/* A multi-second starvation starts a fresh timestamp epoch rather than
	 * slowly feeding late audio into OBS. */
	wall += 4 * NS_PER_SEC;
	int64_t recovered =
		output_timeline_next(&timeline, wall, frame, true);
	CHECK(recovered == wall + OUTPUT_TIMELINE_LEAD_NS);
	CHECK(recovered > second);
	CHECK(timeline.resync_count == 1);

	int64_t after = output_timeline_next(
		&timeline, wall + 20 * NS_PER_MS, frame, false);
	CHECK(after == recovered + frame);
	CHECK(after >= wall + 20 * NS_PER_MS +
			      OUTPUT_TIMELINE_MIN_LEAD_NS);

	/* Scheduler lateness is also a discontinuity even when the transport
	 * did not explicitly report one. */
	wall += NS_PER_SEC;
	int64_t late = output_timeline_next(&timeline, wall, frame, false);
	CHECK(late == wall + OUTPUT_TIMELINE_LEAD_NS);
	CHECK(timeline.resync_count == 2);

	CHECK(output_timeline_next(NULL, wall, frame, false) == 0);
	output_timeline_reset(NULL);
}

static void test_buffer_invalid_input(void)
{
	struct audio_buffer buffer;
	CHECK(audio_buffer_init(&buffer));
	CHECK(!audio_buffer_init(NULL));
	struct audio_buf_frame *out = NULL;
	CHECK(!audio_buffer_pop(NULL, &out));
	CHECK(!audio_buffer_pop(&buffer, NULL));
	audio_buffer_note_underrun(NULL);
	audio_buffer_note_discontinuity(NULL);
	audio_buffer_trim_to_target(NULL);

	const uint8_t byte = 1;
	const uint8_t *planes[AUDIO_BUF_MAX_PLANES] = {0};
	size_t sizes[AUDIO_BUF_MAX_PLANES] = {0};
	planes[0] = &byte;
	sizes[0] = 1;

	CHECK(!audio_buffer_push(NULL, planes, sizes, 1, 48000, 1, 1, 1, 0, 0));
	CHECK(!audio_buffer_push(&buffer, NULL, sizes, 1, 48000, 1, 1, 1, 0,
				 0));
	CHECK(!audio_buffer_push(&buffer, planes, sizes, 0, 48000, 1, 1, 1, 0,
				 0));
	CHECK(!audio_buffer_push(&buffer, planes, sizes, 1, 0, 1, 1, 1, 0, 0));
	CHECK(!audio_buffer_push(&buffer, planes, sizes, 1, 48000, 9, 1, 1, 0,
				 0));

	sizes[0] = 0; /* pointer/size mismatch */
	CHECK(!audio_buffer_push(&buffer, planes, sizes, 1, 48000, 1, 1, 1, 0,
				 0));
	sizes[0] = AUDIO_BUF_MAX_FRAME_BYTES + 1U;
	CHECK(!audio_buffer_push(&buffer, planes, sizes, 1, 48000, 1, 1, 1, 0,
				 0));

	sizes[0] = 1;
	CHECK(audio_buffer_push(&buffer, planes, sizes, 960, 48000, 1, 1, 1,
				INT64_MIN, INT64_MIN));
	CHECK(audio_buffer_push(&buffer, planes, sizes, 960, 48000, 1, 1, 1,
				INT64_MIN + 1, INT64_MAX));
	struct audio_buffer_stats stats;
	audio_buffer_get_stats(&buffer, &stats);
	CHECK(stats.target_ns == 750 * NS_PER_MS);
	CHECK(stats.max_ns == 1000 * NS_PER_MS);

	audio_buffer_free(&buffer);
}

static void test_buffer_batched_delivery_target(void)
{
	struct audio_buffer buffer;
	CHECK(audio_buffer_init(&buffer));

	/* A 600ms demux delivery gap must survive the many near-zero gaps in
	 * the following batch; average jitter alone would decay to ~40ms and
	 * repeatedly start playback with too little protection. */
	for (uint64_t i = 0; i < 30; i++)
		CHECK(push_mono(&buffer, i, 960, (int64_t)i * 20 * NS_PER_MS,
				0));
	CHECK(push_mono(&buffer, 30, 960, 30 * 20 * NS_PER_MS,
			600 * NS_PER_MS));
	for (uint64_t i = 31; i < 60; i++)
		CHECK(push_mono(&buffer, i, 960, (int64_t)i * 20 * NS_PER_MS,
				600 * NS_PER_MS));

	struct audio_buffer_stats stats;
	audio_buffer_get_stats(&buffer, &stats);
	CHECK(stats.delivery_gap_ns == 600 * NS_PER_MS);
	CHECK(stats.target_ns >= 640 * NS_PER_MS);
	CHECK(stats.max_ns >= 890 * NS_PER_MS);
	audio_buffer_free(&buffer);
}

static void test_buffer_discontinuity_forgets_outage_gap(void)
{
	struct audio_buffer buffer;
	CHECK(audio_buffer_init(&buffer));

	CHECK(push_mono(&buffer, 0, 960, 0, 0));
	CHECK(push_mono(&buffer, 1, 960, 20 * NS_PER_MS, 600 * NS_PER_MS));
	audio_buffer_note_underrun(&buffer);

	struct audio_buffer_stats stats;
	audio_buffer_get_stats(&buffer, &stats);
	CHECK(stats.delivery_gap_ns == 600 * NS_PER_MS);
	CHECK(stats.target_ns >= 640 * NS_PER_MS);

	/* A confirmed outage starts a fresh delivery epoch. The underrun cushion
	 * remains, but the outage itself must not pin the target at 750 ms. */
	audio_buffer_note_discontinuity(&buffer);
	audio_buffer_get_stats(&buffer, &stats);
	CHECK(stats.delivery_gap_ns == 0);
	CHECK(stats.jitter_ns == 0);
	CHECK(stats.target_ns == 140 * NS_PER_MS);

	CHECK(push_mono(&buffer, 2, 960, 40 * NS_PER_MS, 5 * NS_PER_SEC));
	audio_buffer_get_stats(&buffer, &stats);
	CHECK(stats.delivery_gap_ns == 0);
	CHECK(stats.target_ns < 140 * NS_PER_MS);
	CHECK(stats.target_ns >= 139 * NS_PER_MS);

	audio_buffer_free(&buffer);
}

static void test_large_storage_is_released(void)
{
	struct audio_buffer buffer;
	CHECK(audio_buffer_init(&buffer));
	audio_buffer_set_minimum(&buffer, 0);

	size_t large_size = 64U * 1024U + 1U;
	uint8_t *large = malloc(large_size);
	CHECK(large != NULL);
	if (!large) {
		audio_buffer_free(&buffer);
		return;
	}
	memset(large, 0x5a, large_size);

	const uint8_t *planes[AUDIO_BUF_MAX_PLANES] = {0};
	size_t sizes[AUDIO_BUF_MAX_PLANES] = {0};
	planes[0] = large;
	sizes[0] = large_size;
	CHECK(audio_buffer_push(&buffer, planes, sizes, 960, 48000, 1, 1, 1, 0,
				0));
	struct audio_buf_frame *out = NULL;
	CHECK(audio_buffer_pop(&buffer, &out));
	CHECK(out != NULL);
	CHECK(out->data_capacity[0] == large_size);

	uint8_t small = 1;
	planes[0] = &small;
	sizes[0] = 1;
	CHECK(audio_buffer_push(&buffer, planes, sizes, 960, 48000, 1, 1, 1,
				20 * NS_PER_MS, 20 * NS_PER_MS));
	CHECK(audio_buffer_pop(&buffer, &out));

	size_t retained = 0;
	for (int i = 0; i < AUDIO_BUF_MAX_FRAMES; i++)
		for (int p = 0; p < AUDIO_BUF_MAX_PLANES; p++)
			retained += buffer.frames[i].data_capacity[p];
	CHECK(retained <= 64U * 1024U);

	free(large);
	audio_buffer_free(&buffer);
}

struct buffer_stress {
	struct audio_buffer buffer;
	atomic_bool producer_done;
	atomic_uint_fast64_t last_seen;
	atomic_uint_fast64_t pops;
};

static void *stress_producer(void *opaque)
{
	struct buffer_stress *stress = opaque;
	for (uint64_t i = 1; i <= 10000; i++)
		CHECK(push_mono(&stress->buffer, i, 480,
				(int64_t)i * 10 * NS_PER_MS,
				(int64_t)i * 10 * NS_PER_MS));
	atomic_store_explicit(&stress->producer_done, true,
			      memory_order_release);
	return NULL;
}

static void *stress_consumer(void *opaque)
{
	struct buffer_stress *stress = opaque;
	for (;;) {
		struct audio_buf_frame *frame = NULL;
		if (audio_buffer_pop(&stress->buffer, &frame)) {
			uint64_t value = 0;
			memcpy(&value, frame->data[0], sizeof(value));
			uint64_t previous = atomic_load_explicit(
				&stress->last_seen, memory_order_relaxed);
			CHECK(value > previous);
			atomic_store_explicit(&stress->last_seen, value,
					      memory_order_relaxed);
			atomic_fetch_add_explicit(&stress->pops, 1,
						  memory_order_relaxed);
			continue;
		}

		struct audio_buffer_stats stats;
		audio_buffer_get_stats(&stress->buffer, &stats);
		if (atomic_load_explicit(&stress->producer_done,
					 memory_order_acquire) &&
		    stats.count == 0)
			break;
	}
	return NULL;
}

static void *stress_observer(void *opaque)
{
	struct buffer_stress *stress = opaque;
	do {
		struct audio_buffer_stats stats;
		audio_buffer_get_stats(&stress->buffer, &stats);
		CHECK(stats.count >= 0);
		CHECK(stats.count <= AUDIO_BUF_MAX_FRAMES);
		CHECK(stats.level_ns >= 0);
		CHECK(stats.target_ns >= 0);
	} while (!atomic_load_explicit(&stress->producer_done,
				       memory_order_acquire));
	return NULL;
}

static void test_buffer_concurrency(void)
{
	struct buffer_stress stress;
	memset(&stress, 0, sizeof(stress));
	CHECK(audio_buffer_init(&stress.buffer));
	atomic_init(&stress.producer_done, false);
	atomic_init(&stress.last_seen, 0);
	atomic_init(&stress.pops, 0);

	pthread_t producer;
	pthread_t consumer;
	pthread_t observer;
	CHECK(pthread_create(&producer, NULL, stress_producer, &stress) == 0);
	CHECK(pthread_create(&consumer, NULL, stress_consumer, &stress) == 0);
	CHECK(pthread_create(&observer, NULL, stress_observer, &stress) == 0);
	pthread_join(producer, NULL);
	pthread_join(consumer, NULL);
	pthread_join(observer, NULL);

	CHECK(atomic_load(&stress.pops) > 0);
	audio_buffer_free(&stress.buffer);
}

static void test_clock_rate_and_discontinuity(void)
{
	struct clock_tracker clock;
	CHECK(clock_tracker_init(&clock));
	CHECK(!clock_tracker_init(NULL));

	const int64_t pts_step = 20 * NS_PER_MS;
	const int64_t wall_step = 20408163; /* 20 ms / 0.98 */
	for (int64_t i = 0; i < 700; i++)
		clock_tracker_record(&clock, i * pts_step, i * wall_step);

	double rate = clock_tracker_get_smoothed_rate(&clock);
	CHECK(rate > 0.978);
	CHECK(rate < 0.985);

	/* A backwards PTS starts a clean epoch rather than pinning the rate
	 * clamp for the old five-second window. */
	int64_t wall = 700 * wall_step;
	clock_tracker_record(&clock, 0, wall + pts_step);
	for (int64_t i = 1; i < 700; i++)
		clock_tracker_record(&clock, i * pts_step,
				     wall + (i + 1) * pts_step);
	rate = clock_tracker_get_smoothed_rate(&clock);
	CHECK(rate > 0.995);
	CHECK(rate < 1.005);

	int64_t adjusted =
		clock_tracker_adjust_timestamp(&clock, 701 * pts_step);
	CHECK(adjusted > 0);
	clock_tracker_free(&clock);
}

static void test_clock_batched_delivery(void)
{
	struct clock_tracker clock;
	CHECK(clock_tracker_init(&clock));

	/* MPEG-TS and network receivers often release roughly 600 ms of audio
	 * in a tight group. The rate estimator must weight each delivery group
	 * by elapsed wall time, not by the number of decoded frames in it. */
	const int64_t frame_ns = 1024 * NS_PER_SEC / 44100;
	const int64_t batch_ns = 600 * NS_PER_MS;
	const int64_t decode_step_ns = 200000;
	int64_t previous_batch = -1;
	int64_t within_batch = 0;
	for (int64_t i = 0; i < 5200; i++) {
		int64_t pts = i * frame_ns;
		int64_t ideal_wall = (int64_t)((long double)pts / 0.93L);
		int64_t batch =
			((ideal_wall + batch_ns - 1) / batch_ns) * batch_ns;
		if (batch != previous_batch) {
			previous_batch = batch;
			within_batch = 0;
		}
		clock_tracker_record(&clock, pts,
				     batch + within_batch * decode_step_ns);
		within_batch++;
	}

	double rate = clock_tracker_get_smoothed_rate(&clock);
	CHECK(rate > 0.925);
	CHECK(rate < 0.935);
	clock_tracker_free(&clock);
}

struct clock_stress {
	struct clock_tracker clock;
	atomic_bool done;
};

static void *clock_writer(void *opaque)
{
	struct clock_stress *stress = opaque;
	for (int64_t i = 0; i < 10000; i++)
		clock_tracker_record(&stress->clock, i * 2500000, i * 2500100);
	atomic_store_explicit(&stress->done, true, memory_order_release);
	return NULL;
}

static void *clock_reader(void *opaque)
{
	struct clock_stress *stress = opaque;
	do {
		double rate = clock_tracker_get_smoothed_rate(&stress->clock);
		CHECK(rate >= 0.90);
		CHECK(rate <= 1.10);
		(void)clock_tracker_get_drift(&stress->clock);
		(void)clock_tracker_adjust_timestamp(&stress->clock, 1);
	} while (!atomic_load_explicit(&stress->done, memory_order_acquire));
	return NULL;
}

static void test_clock_concurrency(void)
{
	struct clock_stress stress;
	memset(&stress, 0, sizeof(stress));
	CHECK(clock_tracker_init(&stress.clock));
	atomic_init(&stress.done, false);

	pthread_t writer;
	pthread_t reader;
	CHECK(pthread_create(&writer, NULL, clock_writer, &stress) == 0);
	CHECK(pthread_create(&reader, NULL, clock_reader, &stress) == 0);
	pthread_join(writer, NULL);
	pthread_join(reader, NULL);

	clock_tracker_free(&stress.clock);
}

int main(void)
{
	fprintf(stderr, "test_buffer_basic\n");
	test_buffer_basic();
	fprintf(stderr, "test_buffer_trim_and_adaptation\n");
	test_buffer_trim_and_adaptation();
	fprintf(stderr, "test_buffer_reprime_after_underrun\n");
	test_buffer_reprime_after_underrun();
	fprintf(stderr, "test_buffer_invalid_input\n");
	test_buffer_invalid_input();
	fprintf(stderr, "test_buffer_batched_delivery_target\n");
	test_buffer_batched_delivery_target();
	fprintf(stderr, "test_buffer_discontinuity_forgets_outage_gap\n");
	test_buffer_discontinuity_forgets_outage_gap();
	fprintf(stderr, "test_large_storage_is_released\n");
	test_large_storage_is_released();
	fprintf(stderr, "test_buffer_concurrency\n");
	test_buffer_concurrency();
	fprintf(stderr, "test_clock_rate_and_discontinuity\n");
	test_clock_rate_and_discontinuity();
	fprintf(stderr, "test_clock_batched_delivery\n");
	test_clock_batched_delivery();
	fprintf(stderr, "test_clock_concurrency\n");
	test_clock_concurrency();
	fprintf(stderr, "test_output_timeline_recovery\n");
	test_output_timeline_recovery();

	if (failures != 0) {
		fprintf(stderr, "%d test assertion(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	printf("core-tests: all assertions passed\n");
	return EXIT_SUCCESS;
}
