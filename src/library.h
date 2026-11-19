// Shmees Media Center: the media library. A background thread scans the library folders for video files, reads
// Kodi-style .nfo files and finds poster and backdrop images beside them; the index is saved so the next start shows
// the library at once while it rescans. What the viewer did (progress, watched, favourites) is kept apart, by path, so a
// rescan never loses it.
#ifndef SMC_LIBRARY_H
#define SMC_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>

enum {
	LIB_PATH = 260,
	LIB_TITLE = 128,
	LIB_GENRE = 64,
	LIB_PLOT = 900,
	LIB_MAX_ROOTS = 8,
	LIB_MAX_ITEMS = 6000,
};

typedef enum {
	MEDIA_MOVIE = 0,
	MEDIA_EPISODE = 1,
} MediaKind;

typedef struct {
	char path[LIB_PATH];
	// A movie's title, or an episode's own title when it has one.
	char title[LIB_TITLE];
	// An episode's show; empty for a movie.
	char show[LIB_TITLE];
	char genre[LIB_GENRE];
	char plot[LIB_PLOT];
	char poster[LIB_PATH];
	char backdrop[LIB_PATH];
	// "MP4", "MKV" and so on, from the extension.
	char container[8];
	uint8_t kind;
	uint8_t season;
	uint16_t episode;
	uint16_t year;
	uint16_t runtime_min;
	// Which library root it came from, an index into the scan's roots.
	uint8_t root;
	uint64_t size_bytes;
	// When the file was last written, seconds since 1970: what "Recently Added" sorts by.
	int64_t added;
} MediaItem;

// The viewer's own state for one file.
typedef struct {
	uint32_t position_s;
	uint32_t duration_s;
	bool watched;
	bool favorite;
	// Seconds since 1970 it was last played; 0 never.
	int64_t last_played;
} MediaUser;

typedef struct {
	char name[LIB_TITLE];
	char poster[LIB_PATH];
	char backdrop[LIB_PATH];
	char plot[LIB_PLOT];
	char genre[LIB_GENRE];
	uint16_t year;
	// Episodes in library order (show, season, episode), their indices into the library's items.
	uint32_t first;
	uint32_t count;
	uint32_t seasons;
	int64_t added;
} MediaShow;

typedef struct {
	MediaItem *items;
	MediaUser *user;
	uint32_t count;
	// Movies in title order, indices into items.
	uint32_t *movies;
	uint32_t movie_count;
	// Episodes sorted show, season, episode; shows point into it.
	uint32_t *episodes;
	uint32_t episode_count;
	MediaShow *shows;
	uint32_t show_count;
	// For each item: its show, or UINT32_MAX, and its place in episodes.
	uint32_t *show_of;
	uint32_t *episode_at;
} Library;

typedef struct {
	char path[LIB_PATH];
	bool enabled;
	// Whether the drive answered when last scanned, and what was found there.
	bool present;
	uint32_t found;
} LibraryRoot;

typedef struct {
	bool scanning;
	uint32_t folders;
	uint32_t files;
	uint32_t videos;
	uint32_t nfos;
	uint32_t posters;
	uint32_t ms;
	char current[LIB_PATH];
	char error[128];
} ScanStatus;

// Loads the saved index and user data from data_dir (game:/data), so the library shows before any scan.
void library_init(const char *data_dir);
void library_set_roots(const LibraryRoot *roots, uint32_t count);
void library_get_roots(LibraryRoot *roots, uint32_t *count);
// Starts a scan on a thread of its own; a scan already running is left to finish.
void library_scan_start(void);
void library_scan_status(ScanStatus *status);
// Whether a finished scan's results are waiting.
bool library_pending(void);
// Takes a finished scan's results, if there are any, on the UI thread. True when the library changed.
bool library_poll(void);
// The library as it is; valid until the next library_poll that returns true.
const Library *library_get(void);
MediaUser *library_user(uint32_t item);
// Writes user data out now (it is also written after each change through library_user_changed).
void library_user_changed(void);
void library_save_user(void);
// Items with progress and not finished, most recent first; returns how many, up to max.
uint32_t library_continue_watching(uint32_t *out, uint32_t max);
// Newest files first, an episode standing for its show once.
uint32_t library_recently_added(uint32_t *out, uint32_t max, bool shows_once);
uint32_t library_favorites(uint32_t *out, uint32_t max);
// Case-insensitive search of titles, shows and genres; returns item indices, movies and the first episode of each show.
uint32_t library_search(const char *query, uint32_t *out, uint32_t max);
// The episode after this one in its show, or UINT32_MAX.
uint32_t library_next_episode(uint32_t item);
// The show an episode belongs to, or UINT32_MAX.
uint32_t library_show_of(uint32_t item);
// "1h 52m", "48m", or "" for none.
void library_format_runtime(uint32_t minutes, char *out, uint32_t size);

#endif
