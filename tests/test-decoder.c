#include "stream-decoder.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(condition)                                                     \
	do {                                                                  \
		if (!(condition)) {                                              \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n",             \
				__FILE__, __LINE__, #condition);                   \
			failures++;                                               \
		}                                                             \
	} while (0)

struct decode_observer {
	int video_frames;
	int audio_frames;
	int stop_calls;
	int64_t last_video_pts;
	int64_t last_audio_pts;
};

static void on_video(void *opaque, struct decoded_video_frame *frame)
{
	struct decode_observer *observer = opaque;
	CHECK(frame != NULL);
	CHECK(frame->width > 0);
	CHECK(frame->height > 0);
	CHECK(frame->data[0] != NULL);
	if (observer->video_frames > 0)
		CHECK(frame->pts_ns >= observer->last_video_pts);
	observer->last_video_pts = frame->pts_ns;
	observer->video_frames++;
}

static void on_audio(void *opaque, struct decoded_audio_frame *frame)
{
	struct decode_observer *observer = opaque;
	CHECK(frame != NULL);
	CHECK(frame->frames > 0);
	CHECK(frame->sample_rate > 0);
	CHECK(frame->channels > 0);
	CHECK(frame->channels <= 8);
	CHECK(frame->data[0] != NULL);
	CHECK(frame->data_size[0] > 0);
	if (observer->audio_frames > 0)
		CHECK(frame->pts_ns >= observer->last_audio_pts);
	observer->last_audio_pts = frame->pts_ns;
	observer->audio_frames++;
}

static void on_stop(void *opaque)
{
	struct decode_observer *observer = opaque;
	observer->stop_calls++;
}

static void test_invalid_inputs(void)
{
	CHECK(stream_decoder_create(NULL) == NULL);

	int result = 0;
	struct stream_decoder_info empty = {
		.open_result = &result,
	};
	CHECK(stream_decoder_create(&empty) == NULL);
	CHECK(result == AVERROR(EINVAL));

	struct stream_decoder_info bad_format = {
		.url = "does-not-exist",
		.format_name = "definitely-not-a-demuxer",
		.open_result = &result,
	};
	CHECK(stream_decoder_create(&bad_format) == NULL);
	CHECK(result == AVERROR_DEMUXER_NOT_FOUND);

	struct stream_decoder_info bad_options = {
		.url = "does-not-exist",
		.ffmpeg_options = "missing-value",
		.open_result = &result,
	};
	CHECK(stream_decoder_create(&bad_options) == NULL);
	CHECK(result < 0);
}

static void test_decode_file(const char *path)
{
	struct decode_observer observer = {0};
	int result = 0;
	volatile bool abort_flag = false;
	struct stream_decoder_info info = {
		.url = path,
		.abort_flag = &abort_flag,
		.open_result = &result,
		.opaque = &observer,
		.video_cb = on_video,
		.audio_cb = on_audio,
		.stop_cb = on_stop,
	};

	struct stream_decoder *decoder = stream_decoder_create(&info);
	CHECK(decoder != NULL);
	if (!decoder)
		return;

	int iterations = 0;
	while (stream_decoder_read_next(decoder)) {
		iterations++;
		CHECK(iterations < 100000);
		if (iterations >= 100000)
			break;
	}

	fprintf(stderr, "decoded video=%d audio=%d last_video=%" PRId64 "\n",
		observer.video_frames, observer.audio_frames,
		observer.last_video_pts);
	/* The generated two-second, 30 fps stream has delayed B-frames.
	 * Reaching its final 1.967 s PTS proves the EOF path flushed the
	 * delayed tail. */
	CHECK(observer.video_frames >= 20);
	CHECK(observer.last_video_pts >= INT64_C(1950000000));
	CHECK(observer.audio_frames >= 20);
	CHECK(observer.stop_calls == 1);
	CHECK(stream_decoder_should_stop(decoder) == false);
	stream_decoder_destroy(decoder);
}

static void test_corrupt_stream_recovery(const char *path)
{
	struct decode_observer observer = {0};
	int result = 0;
	struct stream_decoder_info info = {
		.url = path,
		.open_result = &result,
		.opaque = &observer,
		.video_cb = on_video,
		.audio_cb = on_audio,
		.stop_cb = on_stop,
	};

	struct stream_decoder *decoder = stream_decoder_create(&info);
	CHECK(decoder != NULL);
	if (!decoder)
		return;

	int iterations = 0;
	while (stream_decoder_read_next(decoder)) {
		iterations++;
		CHECK(iterations < 200000);
		if (iterations >= 200000)
			break;
	}

	fprintf(stderr,
		"corrupt recovery video=%d audio=%d last_video=%" PRId64 "\n",
		observer.video_frames, observer.audio_frames,
		observer.last_video_pts);
	/* The fixture destroys encoded video around seconds 2-3 but contains
	 * clean GOPs through second 6. The old behavior stopped at the first
	 * codec rejection; reaching the tail proves corruption is isolated to
	 * dropped packets/frames and the live transport remains usable. */
	CHECK(observer.video_frames >= 120);
	CHECK(observer.audio_frames >= 180);
	CHECK(observer.last_video_pts >= INT64_C(6000000000));
	CHECK(observer.stop_calls == 1);
	stream_decoder_destroy(decoder);
}

struct abort_open {
	volatile bool abort_flag;
	struct stream_decoder *decoder;
	int result;
};

static void *open_blocked_udp(void *opaque)
{
	struct abort_open *open = opaque;
	struct stream_decoder_info info = {
		.url = "udp://127.0.0.1:39991",
		.format_name = "mpegts",
		.abort_flag = &open->abort_flag,
		.open_result = &open->result,
	};
	open->decoder = stream_decoder_create(&info);
	return NULL;
}

static double monotonic_seconds(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void test_blocking_open_abort(void)
{
	struct abort_open open = {0};
	pthread_t thread;
	double start = monotonic_seconds();
	CHECK(pthread_create(&thread, NULL, open_blocked_udp, &open) == 0);
	usleep(150000);
	os_atomic_set_bool(&open.abort_flag, true);
	pthread_join(thread, NULL);
	double elapsed = monotonic_seconds() - start;

	CHECK(open.decoder == NULL);
	CHECK(open.result < 0);
	CHECK(elapsed < 3.0);
}

static void test_blocking_open_deadline(void)
{
	struct abort_open open = {0};
	struct stream_decoder_info info = {
		.url = "udp://127.0.0.1:39992",
		.format_name = "mpegts",
		.open_timeout_ms = 250,
		.open_result = &open.result,
	};
	double start = monotonic_seconds();
	open.decoder = stream_decoder_create(&info);
	double elapsed = monotonic_seconds() - start;

	CHECK(open.decoder == NULL);
	CHECK(open.result < 0);
	CHECK(elapsed >= 0.15);
	CHECK(elapsed < 3.0);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr,
			"usage: %s TEST_MEDIA CORRUPT_TEST_MEDIA\n", argv[0]);
		return EXIT_FAILURE;
	}

	test_invalid_inputs();
	test_decode_file(argv[1]);
	test_corrupt_stream_recovery(argv[2]);
	test_blocking_open_abort();
	test_blocking_open_deadline();
	stream_decoder_log_protocols();
	stream_decoder_global_cleanup();

	if (failures != 0) {
		fprintf(stderr, "%d decoder assertion(s) failed\n", failures);
		return EXIT_FAILURE;
	}
	printf("decoder-tests: all assertions passed\n");
	return EXIT_SUCCESS;
}
