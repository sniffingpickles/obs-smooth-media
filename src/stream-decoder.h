#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4204)
#endif

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

/*
 * Stream Decoder — FFmpeg-based demuxer/decoder for live streams.
 *
 * This replaces OBS's built-in media-playback with a design that:
 * - Runs av_read_frame on a dedicated I/O thread (non-blocking to playback)
 * - Decodes audio/video on the playback thread
 * - Provides decoded frames with accurate PTS to the caller
 */

struct decoded_video_frame {
	uint8_t *data[4];
	int linesize[4];
	int width;
	int height;
	int64_t pts_ns;
	int format;         /* AVPixelFormat */
	int colorspace;     /* AVColorSpace */
	int color_range;    /* AVColorRange */
	int color_trc;      /* AVColorTransferCharacteristic */
	int color_primaries;
	bool keyframe;
	uint16_t max_luminance;
};

struct decoded_audio_frame {
	uint8_t *data[8];
	size_t data_size[8];
	uint32_t frames;        /* number of samples */
	uint32_t sample_rate;
	uint32_t channels;
	int64_t pts_ns;
	int format;             /* AVSampleFormat */
};

typedef void (*stream_video_cb)(void *opaque, struct decoded_video_frame *frame);
typedef void (*stream_audio_cb)(void *opaque, struct decoded_audio_frame *frame);
typedef void (*stream_stop_cb)(void *opaque);

struct stream_decoder_info {
	const char *url;
	const char *format_name;
	const char *ffmpeg_options;
	int buffering_bytes;
	bool hardware_decoding;

	void *opaque;
	stream_video_cb video_cb;
	stream_audio_cb audio_cb;
	stream_stop_cb stop_cb;
};

struct stream_decode_ctx {
	AVStream *stream;
	AVCodecContext *decoder;
	const AVCodec *codec;
	AVFrame *frame;
	AVFrame *sw_frame;
	AVFrame *hw_frame;
	AVBufferRef *hw_ctx;
	int64_t last_pts_ns;
	int64_t next_pts_ns;
	bool got_first_keyframe;
	bool audio;
	bool hw;
	uint16_t max_luminance;
};

struct stream_decoder {
	AVFormatContext *fmt_ctx;

	struct stream_decode_ctx video;
	struct stream_decode_ctx audio;
	bool has_video;
	bool has_audio;

	char *url;
	char *format_name;
	char *ffmpeg_options;
	int buffering;
	bool hw_decode;

	void *opaque;
	stream_video_cb video_cb;
	stream_audio_cb audio_cb;
	stream_stop_cb stop_cb;

	/* Threading */
	volatile bool running;
	volatile bool kill;

	/* Interrupt callback state */
	uint64_t interrupt_poll_ts;
};

/* Create and open the stream. Returns NULL on failure. */
struct stream_decoder *stream_decoder_create(const struct stream_decoder_info *info);

/* Destroy and free all resources */
void stream_decoder_destroy(struct stream_decoder *sd);

/* Read and decode the next frame(s). Returns false on EOF or error.
 * Calls video_cb and/or audio_cb for each decoded frame.
 * This is designed to be called in a loop from the media thread. */
bool stream_decoder_read_next(struct stream_decoder *sd);

/* Signal the decoder to stop (thread-safe) */
void stream_decoder_request_stop(struct stream_decoder *sd);

/* Check if decoder has been signaled to stop */
bool stream_decoder_should_stop(const struct stream_decoder *sd);
