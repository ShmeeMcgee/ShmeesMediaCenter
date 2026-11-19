// Shmees Media Center: playback. FFmpeg (libavformat, libavcodec, libswresample; LGPL 2.1) reads and decodes on the
// CPU, as homebrew has no way to the console's video hardware: one thread reads the file and decodes audio and video,
// FFmpeg's frame threads spread the video decoding over four more hardware threads, and an audio thread feeds the
// kernel's audio driver. The UI thread takes the frame due now, uploads its planes and draws them through video.frag.
#ifndef SMC_PLAYER_H
#define SMC_PLAYER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	PLAYER_IDLE,
	PLAYER_OPENING,
	PLAYER_BUFFERING,
	PLAYER_PLAYING,
	PLAYER_PAUSED,
	PLAYER_ENDED,
	PLAYER_FAILED,
} PlayerState;

typedef struct {
	PlayerState state;
	double position;
	double duration;
	bool has_video;
	bool has_audio;
	int width;
	int height;
	double fps;
	char video_codec[32];
	char audio_codec[32];
	char error[160];
	// Since opening or the last seek.
	uint32_t decoded;
	uint32_t shown;
	uint32_t dropped;
	uint32_t audio_underruns;
	// Over the last second.
	float decode_fps;
	uint32_t queued_frames;
	float audio_buffered_s;
	int threads;
	// How much the decoder gives up to keep up: 0 nothing, 1 deblocking on B-frames, 2 all deblocking, 3 that and
	// B-frames themselves; and its name.
	int level;
	char level_name[48];
} PlayerStatus;

// Starts playing a file from start_s seconds; false if it couldn't start (player_status says why).
bool player_open(const char *path, double start_s, uint32_t volume_percent);
void player_close(void);
void player_set_paused(bool paused);
void player_seek(double position_s);
void player_set_volume(uint32_t volume_percent);
void player_status(PlayerStatus *status);
// Once a frame on the UI thread, before drawing: takes the frame due now to the textures.
void player_update(void);
// Draws the current frame fitted into the rectangle, keeping its shape; false if there's no frame yet.
bool player_draw(float x, float y, float w, float h);

// FFmpeg's AltiVec code for the files opened after: off by default. VMX128 lacks some of VMX's integer instructions,
// so it's for consoles the VMX probe cleared (app.c checks its results).
void player_set_vector(bool on);
bool player_vector(void);

// How fast the console decodes a file's video: decodes as fast as it can for up to seconds, on the same threads
// playback uses, with nothing shown. Runs on its own thread; poll player_benchmark_result.
enum {
	// Checksums of the first pictures, to compare runs with and without the vector unit.
	PLAYER_BENCH_CHECKSUMS = 90,
};

typedef struct {
	bool running;
	bool done;
	char file[64];
	char label[48];
	int threads;
	int level;
	bool vector;
	uint32_t checksums[PLAYER_BENCH_CHECKSUMS];
	uint32_t checksum_count;
	char codec[32];
	int width;
	int height;
	double file_fps;
	uint32_t frames;
	double seconds;
	double fps;
	char error[128];
} PlayerBenchmark;

// threads decoding threads, quality level as PlayerStatus.level; the label names the run in the log.
int player_decode_threads(void);
bool player_benchmark_start(const char *path, double seconds, int threads, int level, bool vector, const char *label);
void player_benchmark_result(PlayerBenchmark *result);

#endif
