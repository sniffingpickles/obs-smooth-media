#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <util/threading.h>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4204)
#endif

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#ifdef _MSC_VER
#pragma warning(pop)
#endif

/*
 * Stream Decoder — FFmpeg-based demuxer/decoder for live streams.
 *
 * This replaces OBS's built-in media-playback with a design that:
 * - Demuxes (av_read_frame) and decodes on a single media thread, driven
 *   by stream_decoder_read_next() called in a loop by the caller
 * - Pushes decoded frames to the caller via callbacks; pacing/jitter
 *   absorption is handled downstream by the audio jitter buffer, not here
 * - Provides decoded frames with accurate PTS (in nanoseconds)
 * - Optionally decodes video on the GPU (hardware acceleration) with an
 *   automatic fall back to software decoding when unavailable
 */

struct decoded_video_frame {
	uint8_t *data[4];
	int linesize[4];
	int width;
	int height;
	int64_t pts_ns;
	int format;      /* AVPixelFormat */
	int colorspace;  /* AVColorSpace */
	int color_range; /* AVColorRange */
	int color_trc;   /* AVColorTransferCharacteristic */
	int color_primaries;
	bool keyframe;
	uint16_t max_luminance;
};

struct decoded_audio_frame {
	uint8_t *data[8];
	size_t data_size[8];
	uint32_t frames; /* number of samples */
	uint32_t sample_rate;
	uint32_t channels;
	int64_t pts_ns;
	int format; /* AVSampleFormat */
};

typedef void (*stream_video_cb)(void *opaque,
				struct decoded_video_frame *frame);
typedef void (*stream_audio_cb)(void *opaque,
				struct decoded_audio_frame *frame);
typedef void (*stream_stop_cb)(void *opaque);

struct stream_decoder_info {
	const char *url;
	const char *format_name;
	const char *ffmpeg_options;
	int buffering_bytes;
	bool hardware_decoding;

	/* Hard fallback deadline for open/probe, in milliseconds. Zero selects
	 * the production default. Protocol-specific timeout options are not
	 * consistently honored by every FFmpeg transport (notably a RIST
	 * listener after a destroyed peer), so the interrupt callback enforces
	 * this deadline around open and stream probing. */
	int open_timeout_ms;

	/* Optional external abort flag. The interrupt callback aborts blocking
	 * I/O (including avformat_open_input) when *abort_flag becomes true.
	 * Lets the caller cancel a connection that is still being opened —
	 * before stream_decoder_create() has even returned a handle. */
	volatile bool *abort_flag;

	/* Optional: on failure, the AVERROR code is written here so the caller
	 * can report exactly why the open failed (connection refused, server
	 * error, no stream, timeout, ...). */
	int *open_result;

	void *opaque;
	stream_video_cb video_cb;
	stream_audio_cb audio_cb;
	stream_stop_cb stop_cb;
};

struct stream_decode_ctx {
	AVStream *stream;
	AVCodecContext *decoder;
	const AVCodec *codec;
	AVFrame *frame;      /* receives decoded frames (may be GPU memory) */
	AVFrame *sw_frame;   /* hw mode: download target for GPU frames */
	AVBufferRef *hw_ctx; /* hw device context (NULL in software mode) */
	enum AVPixelFormat hw_pix_fmt; /* the GPU surface format to negotiate */
	int64_t last_pts_ns;
	int64_t next_pts_ns;
	bool got_first_keyframe;
	bool corrupt_video_logged;
	bool audio;
	bool hw; /* true once hardware decoding is active */
	uint16_t max_luminance;
	int64_t last_error_log_us;
};

struct stream_decoder {
	AVFormatContext *fmt_ctx;
	AVPacket *packet;

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
	volatile bool *abort_flag; /* external cancel (e.g. source teardown) */
	int64_t interrupt_deadline_us; /* media-thread open/probe deadline */
};

/* Build the FFmpeg options used to open an input. Exposed so protocol option
 * defaults can be regression-tested without making a network connection. */
int stream_decoder_prepare_input_options(AVDictionary **options,
					 const char *url, int buffering_bytes,
					 const char *ffmpeg_options);

/* Create and open the stream. Returns NULL on failure. */
struct stream_decoder *
stream_decoder_create(const struct stream_decoder_info *info);

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

/* Release process-global FFmpeg network state. Call once on module unload. */
void stream_decoder_global_cleanup(void);

/* Log the FFmpeg input protocols available at runtime (rtmp/srt/rist/...).
 * Useful to confirm whether the host OBS's FFmpeg includes librist. */
void stream_decoder_log_protocols(void);
