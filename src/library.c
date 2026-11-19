// Shmees Media Center: the media library (library.h).

#define _GNU_SOURCE
#include "library.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "log.h"
#include "threads.h"

enum {
	// How deep below a root the scan goes: Movies/Film/Extras, TV Shows/Show/Season 1/Episode.
	MAX_DEPTH = 4,
	MAX_ENTRIES = 2048,
	NFO_BYTES = 64 * 1024,
	INDEX_VERSION = 2,
	MAX_LINE = 4096,
};

static const char *const VIDEO_EXTENSIONS[] = { "mp4", "m4v", "mov", "mkv", "avi", "wmv", "asf", "mpg", "mpeg", "ts",
	"m2ts", "webm", "flv", "divx", "xvid", "3gp" };
static const char *const IMAGE_EXTENSIONS[] = { "jpg", "jpeg", "png", "tbn", "bmp" };

// Release tags that end a title in a file name.
static const char *const JUNK[] = { "1080p", "720p", "2160p", "480p", "576p", "4k", "uhd", "bluray", "blu-ray", "brrip",
	"bdrip", "webrip", "web-dl", "webdl", "web", "hdtv", "hdrip", "dvdrip", "dvdscr", "x264", "x265", "h264", "h.264",
	"h265", "hevc", "xvid", "divx", "aac", "ac3", "dts", "10bit", "hdr", "remux", "proper", "repack", "extended",
	"unrated", "yify", "yts", "rarbg", "amzn", "nf", "dsnp", "hmax", "ddp5", "dd5", "atmos", "multi" };

typedef struct {
	char name[256];
	bool dir;
} Entry;

// --------------------------------------------------------------------------------------------------------------------
// state

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char data_dir[LIB_PATH] = "game:/data";
static LibraryRoot roots[LIB_MAX_ROOTS];
static uint32_t root_count;

static Library live;
// What a scan made, waiting for library_poll.
static Library *pending;
static ScanStatus status;
static pthread_t scan_thread;
static bool scan_thread_made;

// The user's state lives by path, so items can come and go.
typedef struct {
	char path[LIB_PATH];
	MediaUser user;
} UserRecord;
static UserRecord *user_records;
static uint32_t user_record_count;
static uint32_t user_record_capacity;

// --------------------------------------------------------------------------------------------------------------------
// strings

static void copy(char *dst, size_t size, const char *src)
{
	if (!size)
		return;
	size_t n = strlen(src);
	if (n >= size)
		n = size - 1;
	memcpy(dst, src, n);
	dst[n] = 0;
}

static bool ends_with_ci(const char *s, const char *suffix)
{
	const size_t a = strlen(s);
	const size_t b = strlen(suffix);
	return a >= b && strcasecmp(s + a - b, suffix) == 0;
}

static const char *extension(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot && dot != name ? dot + 1 : "";
}

static bool is_one_of(const char *ext, const char *const *list, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (strcasecmp(ext, list[i]) == 0)
			return true;
	return false;
}

static void trim(char *s)
{
	size_t n = strlen(s);
	while (n && (isspace((unsigned char)s[n - 1]) || s[n - 1] == '-' || s[n - 1] == '(' || s[n - 1] == '[' ||
					s[n - 1] == '.' || s[n - 1] == '_'))
		s[--n] = 0;
	size_t start = 0;
	while (s[start] && (isspace((unsigned char)s[start]) || s[start] == '-' || s[start] == '.' || s[start] == '_'))
		start++;
	if (start)
		memmove(s, s + start, n - start + 1);
	// Runs of spaces to one.
	char *w = s;
	for (const char *r = s; *r; r++) {
		if (*r == ' ' && w > s && w[-1] == ' ')
			continue;
		*w++ = *r;
	}
	*w = 0;
}

static bool is_junk_word(const char *word, size_t len)
{
	char lower[24];
	if (len >= sizeof lower)
		return false;
	for (size_t i = 0; i < len; i++)
		lower[i] = (char)tolower((unsigned char)word[i]);
	lower[len] = 0;
	for (size_t i = 0; i < sizeof JUNK / sizeof JUNK[0]; i++)
		if (strcmp(lower, JUNK[i]) == 0)
			return true;
	return false;
}

// "The.Dark.Knight.2008.1080p.BluRay" -> "The Dark Knight", year 2008.
static void clean_title(const char *raw, char *title, size_t size, uint16_t *year)
{
	char s[LIB_PATH];
	copy(s, sizeof s, raw);
	// Dots and underscores are separators when the name has no spaces, or when they separate most words.
	const bool spaces = strchr(s, ' ') != NULL;
	for (char *p = s; *p; p++)
		if (*p == '_' || (*p == '.' && !spaces))
			*p = ' ';
	// Cut at the first release tag, or at the last year before it ("Blade Runner 2049 (2017)" keeps its 2049). The
	// first word is never cut at: "1917 (2019)", "2001 A Space Odyssey".
	*year = 0;
	char *cut = NULL;
	char *year_at = NULL;
	for (char *p = s; *p;) {
		while (*p && !isalnum((unsigned char)*p))
			p++;
		char *word = p;
		while (*p && (isalnum((unsigned char)*p) || *p == '-'))
			p++;
		const size_t len = (size_t)(p - word);
		if (!len)
			break;
		bool first = true;
		for (const char *q = s; q < word; q++)
			if (isalnum((unsigned char)*q))
				first = false;
		if (first)
			continue;
		if (len == 4 && (word[0] == '1' || word[0] == '2') && isdigit((unsigned char)word[1]) &&
			isdigit((unsigned char)word[2]) && isdigit((unsigned char)word[3])) {
			const int y = atoi(word);
			if (y >= 1900 && y <= 2099) {
				*year = (uint16_t)y;
				year_at = word;
				continue;
			}
		}
		if (is_junk_word(word, len)) {
			cut = word;
			break;
		}
	}
	if (year_at)
		cut = year_at;
	if (cut)
		*cut = 0;
	// Bracketed tags left over: "[Group]" at the start.
	if (s[0] == '[') {
		char *close = strchr(s, ']');
		if (close)
			memmove(s, close + 1, strlen(close + 1) + 1);
	}
	trim(s);
	copy(title, size, s[0] ? s : raw);
}

// Finds S01E02, s1e2 or 1x02. Returns where the tag starts, or NULL.
static const char *find_episode_tag(const char *s, uint8_t *season, uint16_t *episode, const char **after)
{
	for (const char *p = s; *p; p++) {
		if ((p[0] == 'S' || p[0] == 's') && isdigit((unsigned char)p[1]) && (p == s || !isalpha((unsigned char)p[-1]))) {
			const char *q = p + 1;
			int sn = 0;
			int digits = 0;
			while (isdigit((unsigned char)*q) && digits < 3) {
				sn = sn * 10 + (*q++ - '0');
				digits++;
			}
			if (*q == ' ' || *q == '.')
				q++;
			if ((*q == 'E' || *q == 'e') && isdigit((unsigned char)q[1])) {
				q++;
				int ep = 0;
				digits = 0;
				while (isdigit((unsigned char)*q) && digits < 4) {
					ep = ep * 10 + (*q++ - '0');
					digits++;
				}
				// A double episode, S01E01E02 or S01E01-E02: the first.
				while ((*q == 'E' || *q == 'e' || *q == '-') && isdigit((unsigned char)q[1 + (q[1] == 'E' || q[1] == 'e')]))
					for (q++; *q == 'E' || *q == 'e' || isdigit((unsigned char)*q); q++) {
					}
				*season = (uint8_t)(sn > 255 ? 255 : sn);
				*episode = (uint16_t)ep;
				*after = q;
				return p;
			}
		}
		if (isdigit((unsigned char)p[0]) && (p == s || !isalnum((unsigned char)p[-1]))) {
			const char *q = p;
			int sn = 0;
			while (isdigit((unsigned char)*q) && q - p < 2)
				sn = sn * 10 + (*q++ - '0');
			if ((*q == 'x' || *q == 'X') && isdigit((unsigned char)q[1]) && isdigit((unsigned char)q[2])) {
				q++;
				int ep = 0;
				while (isdigit((unsigned char)*q) && ep < 1000)
					ep = ep * 10 + (*q++ - '0');
				if (!isalnum((unsigned char)*q)) {
					*season = (uint8_t)sn;
					*episode = (uint16_t)ep;
					*after = q;
					return p;
				}
			}
		}
	}
	return NULL;
}

// --------------------------------------------------------------------------------------------------------------------
// .nfo files (Kodi's XML), read for a few tags without a full XML parser

static void xml_unescape(char *s)
{
	char *w = s;
	for (const char *r = s; *r;) {
		if (*r == '&') {
			const char *semi = strchr(r, ';');
			if (semi && semi - r < 10) {
				char out = 0;
				if (!strncmp(r, "&amp;", 5))
					out = '&';
				else if (!strncmp(r, "&lt;", 4))
					out = '<';
				else if (!strncmp(r, "&gt;", 4))
					out = '>';
				else if (!strncmp(r, "&quot;", 6))
					out = '"';
				else if (!strncmp(r, "&apos;", 6))
					out = '\'';
				else if (r[1] == '#') {
					const long cp = r[2] == 'x' ? strtol(r + 3, NULL, 16) : strtol(r + 2, NULL, 10);
					// UTF-8 out.
					if (cp > 0 && cp < 0x80)
						out = (char)cp;
					else if (cp >= 0x80 && cp < 0x800) {
						*w++ = (char)(0xC0 | cp >> 6);
						out = (char)(0x80 | (cp & 0x3F));
					} else if (cp >= 0x800 && cp < 0x10000) {
						*w++ = (char)(0xE0 | cp >> 12);
						*w++ = (char)(0x80 | (cp >> 6 & 0x3F));
						out = (char)(0x80 | (cp & 0x3F));
					}
				}
				if (out) {
					*w++ = out;
					r = semi + 1;
					continue;
				}
			}
		}
		*w++ = *r++;
	}
	*w = 0;
}

// The text of the first <tag>...</tag> after from, into out; true if there was one.
static bool xml_tag(const char *xml, const char *tag, char *out, size_t size)
{
	char open[40];
	char close[40];
	snprintf(open, sizeof open, "<%s>", tag);
	snprintf(close, sizeof close, "</%s>", tag);
	const char *a = strstr(xml, open);
	if (!a)
		return false;
	a += strlen(open);
	const char *b = strstr(a, close);
	if (!b)
		return false;
	size_t n = (size_t)(b - a);
	if (n >= size)
		n = size - 1;
	memcpy(out, a, n);
	out[n] = 0;
	// CDATA.
	if (n >= 9 && !strncmp(out, "<![CDATA[", 9)) {
		memmove(out, out + 9, strlen(out + 9) + 1);
		char *end = strstr(out, "]]>");
		if (end)
			*end = 0;
	}
	xml_unescape(out);
	// Whitespace only: a plot keeps its full stop.
	size_t len = strlen(out);
	while (len && isspace((unsigned char)out[len - 1]))
		out[--len] = 0;
	size_t lead = 0;
	while (out[lead] && isspace((unsigned char)out[lead]))
		lead++;
	if (lead)
		memmove(out, out + lead, len - lead + 1);
	return true;
}

// Up to three <genre>s, joined with " · ".
static void xml_genres(const char *xml, char *out, size_t size)
{
	out[0] = 0;
	const char *p = xml;
	for (int n = 0; n < 3; n++) {
		const char *a = strstr(p, "<genre>");
		if (!a)
			break;
		char g[48];
		if (!xml_tag(a, "genre", g, sizeof g))
			break;
		if (out[0])
			strncat(out, " \xC2\xB7 ", size - strlen(out) - 1);
		strncat(out, g, size - strlen(out) - 1);
		p = a + 7;
	}
}

static char *read_small_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	char *buf = malloc(NFO_BYTES + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	const size_t n = fread(buf, 1, NFO_BYTES, f);
	fclose(f);
	buf[n] = 0;
	return buf;
}

typedef struct {
	char title[LIB_TITLE];
	char plot[LIB_PLOT];
	char genre[LIB_GENRE];
	uint16_t year;
	uint16_t runtime;
	int season;
	int episode;
} Nfo;

static bool read_nfo(const char *path, Nfo *nfo)
{
	memset(nfo, 0, sizeof *nfo);
	nfo->season = -1;
	nfo->episode = -1;
	char *xml = read_small_file(path);
	if (!xml)
		return false;
	// A .nfo holding only a URL (IMDb link) has none of these; that's fine.
	char tmp[32];
	xml_tag(xml, "title", nfo->title, sizeof nfo->title);
	if (!xml_tag(xml, "plot", nfo->plot, sizeof nfo->plot))
		xml_tag(xml, "outline", nfo->plot, sizeof nfo->plot);
	xml_genres(xml, nfo->genre, sizeof nfo->genre);
	if (xml_tag(xml, "year", tmp, sizeof tmp))
		nfo->year = (uint16_t)atoi(tmp);
	else if (xml_tag(xml, "premiered", tmp, sizeof tmp) || xml_tag(xml, "aired", tmp, sizeof tmp))
		nfo->year = (uint16_t)atoi(tmp);
	if (xml_tag(xml, "runtime", tmp, sizeof tmp))
		nfo->runtime = (uint16_t)atoi(tmp);
	if (xml_tag(xml, "season", tmp, sizeof tmp))
		nfo->season = atoi(tmp);
	if (xml_tag(xml, "episode", tmp, sizeof tmp))
		nfo->episode = atoi(tmp);
	free(xml);
	return true;
}

// --------------------------------------------------------------------------------------------------------------------
// scanning

typedef struct {
	MediaItem *items;
	uint32_t count;
	uint32_t capacity;
	uint32_t root;
	// The show folder's details, for episodes below it.
	char show_name[LIB_TITLE];
	char show_dir[LIB_PATH];
	ScanStatus progress;
	double start_ms;
} Scan;

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void join(char *out, size_t size, const char *dir, const char *name)
{
	const size_t n = strlen(dir);
	snprintf(out, size, "%s%s%s", dir, n && (dir[n - 1] == '/' || dir[n - 1] == '\\') ? "" : "/", name);
}

// An entry by name, any case.
static const Entry *find_entry(const Entry *entries, uint32_t count, const char *name)
{
	for (uint32_t i = 0; i < count; i++)
		if (!entries[i].dir && strcasecmp(entries[i].name, name) == 0)
			return &entries[i];
	return NULL;
}

// The first of stem + each suffix + an image extension that the folder has.
static bool find_image(const Entry *entries, uint32_t count, const char *dir, const char *stem,
	const char *const *suffixes, size_t suffix_count, char *out, size_t size)
{
	char name[300];
	for (size_t s = 0; s < suffix_count; s++)
		for (size_t e = 0; e < sizeof IMAGE_EXTENSIONS / sizeof IMAGE_EXTENSIONS[0]; e++) {
			snprintf(name, sizeof name, "%s%s.%s", stem, suffixes[s], IMAGE_EXTENSIONS[e]);
			const Entry *found = find_entry(entries, count, name);
			if (found) {
				join(out, size, dir, found->name);
				return true;
			}
		}
	return false;
}

static bool folder_image(const Entry *entries, uint32_t count, const char *dir, const char *const *names, size_t n,
	char *out, size_t size)
{
	static const char *const none[] = { "" };
	for (size_t i = 0; i < n; i++)
		if (find_image(entries, count, dir, names[i], none, 1, out, size))
			return true;
	return false;
}

static const char *const POSTER_NAMES[] = { "poster", "folder", "cover", "movie" };
static const char *const BACKDROP_NAMES[] = { "fanart", "backdrop", "background" };
static const char *const SHOW_POSTER_NAMES[] = { "poster", "folder", "show", "cover" };

static int compare_entries(const void *a, const void *b)
{
	const Entry *x = a;
	const Entry *y = b;
	return strcasecmp(x->name, y->name);
}

static bool is_season_folder(const char *name)
{
	return !strncasecmp(name, "season", 6) || !strncasecmp(name, "series", 6) || !strncasecmp(name, "specials", 8);
}

static bool is_tv_root(const char *path)
{
	const char *slash = strrchr(path, '/');
	const char *name = slash ? slash + 1 : path;
	return strcasestr(name, "tv") != NULL || strcasestr(name, "show") != NULL || strcasestr(name, "series") != NULL;
}

static void add_video(Scan *scan, const char *dir, const char *name, const Entry *entries, uint32_t count,
	uint32_t videos_here, uint32_t depth, bool tv_root)
{
	if (scan->count == scan->capacity) {
		if (scan->capacity >= LIB_MAX_ITEMS)
			return;
		const uint32_t capacity = scan->capacity ? scan->capacity * 2 : 256;
		MediaItem *items = realloc(scan->items, capacity * sizeof *items);
		if (!items)
			return;
		scan->items = items;
		scan->capacity = capacity;
	}
	MediaItem *item = &scan->items[scan->count];
	memset(item, 0, sizeof *item);
	join(item->path, sizeof item->path, dir, name);
	item->root = (uint8_t)scan->root;
	const char *ext = extension(name);
	for (size_t i = 0; i < sizeof item->container - 1 && ext[i]; i++)
		item->container[i] = (char)toupper((unsigned char)ext[i]);

	struct stat st;
	if (stat(item->path, &st) == 0) {
		item->size_bytes = (uint64_t)st.st_size;
		item->added = (int64_t)st.st_mtime;
	}

	char stem[256];
	copy(stem, sizeof stem, name);
	char *dot = strrchr(stem, '.');
	if (dot)
		*dot = 0;

	// Episode or movie.
	uint8_t season = 0;
	uint16_t episode = 0;
	const char *after = NULL;
	const char *tag = find_episode_tag(stem, &season, &episode, &after);
	const bool in_show = scan->show_name[0] != 0;
	if (tag || (in_show && tv_root)) {
		item->kind = MEDIA_EPISODE;
		item->season = tag ? season : 1;
		item->episode = tag ? episode : (uint16_t)(scan->count + 1);
		if (in_show)
			copy(item->show, sizeof item->show, scan->show_name);
		else {
			char raw[256];
			const size_t n = tag ? (size_t)(tag - stem) : strlen(stem);
			memcpy(raw, stem, n);
			raw[n] = 0;
			uint16_t ignored;
			clean_title(raw, item->show, sizeof item->show, &ignored);
		}
		if (tag && after && *after) {
			uint16_t ignored;
			clean_title(after, item->title, sizeof item->title, &ignored);
			// "Show.S01E02.720p.mkv": what follows the tag is release tags, not a title.
			const size_t first_word = strcspn(item->title, " ");
			if (is_junk_word(item->title, first_word))
				item->title[0] = 0;
		}
		if (!item->title[0])
			snprintf(item->title, sizeof item->title, "Episode %u", (unsigned)item->episode);
	} else {
		item->kind = MEDIA_MOVIE;
		// A movie alone in its folder takes the folder's name when that reads better ("Inception (2010)/movie.mp4").
		const char *source = stem;
		const char *slash = strrchr(dir, '/');
		if (videos_here == 1 && depth > 0 && slash && strlen(slash + 1) > 0) {
			uint16_t y = 0;
			char from_folder[LIB_TITLE];
			clean_title(slash + 1, from_folder, sizeof from_folder, &y);
			uint16_t y2 = 0;
			char from_file[LIB_TITLE];
			clean_title(stem, from_file, sizeof from_file, &y2);
			if (y && !y2)
				source = slash + 1;
		}
		clean_title(source, item->title, sizeof item->title, &item->year);
	}

	// Kodi's .nfo: <name>.nfo, or movie.nfo in a movie's own folder.
	char nfo_name[300];
	snprintf(nfo_name, sizeof nfo_name, "%s.nfo", stem);
	const Entry *nfo_entry = find_entry(entries, count, nfo_name);
	if (!nfo_entry && item->kind == MEDIA_MOVIE && videos_here == 1)
		nfo_entry = find_entry(entries, count, "movie.nfo");
	if (nfo_entry) {
		char nfo_path[LIB_PATH];
		join(nfo_path, sizeof nfo_path, dir, nfo_entry->name);
		Nfo nfo;
		if (read_nfo(nfo_path, &nfo)) {
			scan->progress.nfos++;
			if (nfo.title[0])
				copy(item->title, sizeof item->title, nfo.title);
			if (nfo.plot[0])
				copy(item->plot, sizeof item->plot, nfo.plot);
			if (nfo.genre[0])
				copy(item->genre, sizeof item->genre, nfo.genre);
			if (nfo.year)
				item->year = nfo.year;
			if (nfo.runtime)
				item->runtime_min = nfo.runtime;
			if (item->kind == MEDIA_EPISODE && nfo.season >= 0)
				item->season = (uint8_t)nfo.season;
			if (item->kind == MEDIA_EPISODE && nfo.episode >= 0)
				item->episode = (uint16_t)nfo.episode;
		}
	}

	// Artwork beside the file, then the folder's when the folder is the movie's own.
	static const char *const poster_suffixes[] = { "-poster", "", "-thumb", ".poster", "_poster" };
	static const char *const backdrop_suffixes[] = { "-fanart", "-backdrop", ".fanart", "_fanart", "-landscape" };
	bool poster = find_image(entries, count, dir, stem, poster_suffixes, 5, item->poster, sizeof item->poster);
	if (!poster && videos_here == 1 && depth > 0 && item->kind == MEDIA_MOVIE)
		poster = folder_image(entries, count, dir, POSTER_NAMES, 4, item->poster, sizeof item->poster);
	bool backdrop = find_image(entries, count, dir, stem, backdrop_suffixes, 5, item->backdrop, sizeof item->backdrop);
	if (!backdrop && videos_here == 1 && depth > 0 && item->kind == MEDIA_MOVIE)
		backdrop = folder_image(entries, count, dir, BACKDROP_NAMES, 3, item->backdrop, sizeof item->backdrop);
	if (poster)
		scan->progress.posters++;
	(void)backdrop;

	scan->count++;
	scan->progress.videos++;
	roots[scan->root].found++;
}

static void scan_dir(Scan *scan, const char *dir, uint32_t depth, bool tv_root)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	scan->progress.folders++;
	pthread_mutex_lock(&lock);
	copy(status.current, sizeof status.current, dir);
	status.folders = scan->progress.folders;
	status.videos = scan->progress.videos;
	status.files = scan->progress.files;
	pthread_mutex_unlock(&lock);

	Entry *entries = malloc(MAX_ENTRIES * sizeof *entries);
	if (!entries) {
		closedir(d);
		return;
	}
	uint32_t count = 0;
	struct dirent *e;
	while ((e = readdir(d)) && count < MAX_ENTRIES) {
		if (e->d_name[0] == '.' || e->d_name[0] == '$')
			continue;
		copy(entries[count].name, sizeof entries[count].name, e->d_name);
		entries[count].dir = e->d_type == DT_DIR;
		count++;
	}
	closedir(d);
	qsort(entries, count, sizeof *entries, compare_entries);
	scan->progress.files += count;

	uint32_t videos_here = 0;
	for (uint32_t i = 0; i < count; i++)
		if (!entries[i].dir && is_one_of(extension(entries[i].name), VIDEO_EXTENSIONS,
								   sizeof VIDEO_EXTENSIONS / sizeof VIDEO_EXTENSIONS[0]))
			videos_here++;

	for (uint32_t i = 0; i < count; i++) {
		const Entry *entry = &entries[i];
		if (entry->dir)
			continue;
		const char *ext = extension(entry->name);
		if (!is_one_of(ext, VIDEO_EXTENSIONS, sizeof VIDEO_EXTENSIONS / sizeof VIDEO_EXTENSIONS[0]))
			continue;
		// Samples and trailers aren't the film.
		if (strcasestr(entry->name, "sample") && ends_with_ci(entry->name, ext) && strlen(entry->name) < 40 &&
			videos_here > 1)
			continue;
		if (strcasestr(entry->name, "-trailer."))
			continue;
		add_video(scan, dir, entry->name, entries, count, videos_here, depth, tv_root);
	}

	if (depth < MAX_DEPTH) {
		for (uint32_t i = 0; i < count; i++) {
			const Entry *entry = &entries[i];
			if (!entry->dir || !strcasecmp(entry->name, "extras") || !strcasecmp(entry->name, "featurettes") ||
				!strcasecmp(entry->name, "trailers") || !strcasecmp(entry->name, "subs"))
				continue;
			char sub[LIB_PATH];
			join(sub, sizeof sub, dir, entry->name);
			// A folder directly under a TV root is a show; its name names the episodes below.
			const bool show_folder = tv_root && depth == 0;
			char saved_name[LIB_TITLE];
			char saved_dir[LIB_PATH];
			copy(saved_name, sizeof saved_name, scan->show_name);
			copy(saved_dir, sizeof saved_dir, scan->show_dir);
			if (show_folder) {
				uint16_t ignored;
				clean_title(entry->name, scan->show_name, sizeof scan->show_name, &ignored);
				copy(scan->show_dir, sizeof scan->show_dir, sub);
			} else if (depth == 0 && !tv_root && !is_season_folder(entry->name)) {
				scan->show_name[0] = 0;
			}
			scan_dir(scan, sub, depth + 1, tv_root);
			copy(scan->show_name, sizeof scan->show_name, saved_name);
			copy(scan->show_dir, sizeof scan->show_dir, saved_dir);
		}
	}
	free(entries);
}

// --------------------------------------------------------------------------------------------------------------------
// building the views

static const Library *sort_library;

// Titles sort without "The ", "A " and "An ".
static const char *sort_key(const char *title)
{
	if (!strncasecmp(title, "the ", 4))
		return title + 4;
	if (!strncasecmp(title, "a ", 2))
		return title + 2;
	if (!strncasecmp(title, "an ", 3))
		return title + 3;
	return title;
}

static int compare_movies(const void *a, const void *b)
{
	const MediaItem *x = &sort_library->items[*(const uint32_t *)a];
	const MediaItem *y = &sort_library->items[*(const uint32_t *)b];
	const int c = strcasecmp(sort_key(x->title), sort_key(y->title));
	return c ? c : (int)x->year - (int)y->year;
}

static int compare_episodes(const void *a, const void *b)
{
	const MediaItem *x = &sort_library->items[*(const uint32_t *)a];
	const MediaItem *y = &sort_library->items[*(const uint32_t *)b];
	int c = strcasecmp(sort_key(x->show), sort_key(y->show));
	if (c)
		return c;
	if (x->season != y->season)
		return (int)x->season - (int)y->season;
	if (x->episode != y->episode)
		return (int)x->episode - (int)y->episode;
	return strcasecmp(x->path, y->path);
}

static void free_library(Library *lib)
{
	free(lib->items);
	free(lib->user);
	free(lib->movies);
	free(lib->episodes);
	free(lib->shows);
	free(lib->show_of);
	free(lib->episode_at);
	memset(lib, 0, sizeof *lib);
}

// Works out the movie and episode lists and the shows, and finds each show's artwork in its folder.
static bool build_views(Library *lib)
{
	lib->movies = malloc((lib->count + 1) * sizeof *lib->movies);
	lib->episodes = malloc((lib->count + 1) * sizeof *lib->episodes);
	lib->shows = calloc(lib->count + 1, sizeof *lib->shows);
	lib->user = calloc(lib->count + 1, sizeof *lib->user);
	lib->show_of = malloc((lib->count + 1) * sizeof *lib->show_of);
	lib->episode_at = malloc((lib->count + 1) * sizeof *lib->episode_at);
	if (!lib->movies || !lib->episodes || !lib->shows || !lib->user || !lib->show_of || !lib->episode_at)
		return false;
	for (uint32_t i = 0; i < lib->count; i++) {
		lib->show_of[i] = UINT32_MAX;
		lib->episode_at[i] = UINT32_MAX;
	}
	lib->movie_count = 0;
	lib->episode_count = 0;
	for (uint32_t i = 0; i < lib->count; i++) {
		if (lib->items[i].kind == MEDIA_EPISODE)
			lib->episodes[lib->episode_count++] = i;
		else
			lib->movies[lib->movie_count++] = i;
	}
	sort_library = lib;
	qsort(lib->movies, lib->movie_count, sizeof *lib->movies, compare_movies);
	qsort(lib->episodes, lib->episode_count, sizeof *lib->episodes, compare_episodes);
	lib->show_count = 0;
	for (uint32_t i = 0; i < lib->episode_count; i++) {
		const MediaItem *ep = &lib->items[lib->episodes[i]];
		MediaShow *show = lib->show_count ? &lib->shows[lib->show_count - 1] : NULL;
		if (!show || strcasecmp(show->name, ep->show) != 0) {
			show = &lib->shows[lib->show_count++];
			memset(show, 0, sizeof *show);
			copy(show->name, sizeof show->name, ep->show);
			show->first = i;
		}
		if (!show->count || lib->items[lib->episodes[i - 1]].season != ep->season)
			show->seasons++;
		show->count++;
		lib->show_of[lib->episodes[i]] = lib->show_count - 1;
		lib->episode_at[lib->episodes[i]] = i;
		if (ep->added > show->added)
			show->added = ep->added;
	}
	return true;
}

// A show's poster, backdrop and tvshow.nfo, from the folder above its season folders.
static void show_details(Library *lib)
{
	for (uint32_t s = 0; s < lib->show_count; s++) {
		MediaShow *show = &lib->shows[s];
		const MediaItem *first = &lib->items[lib->episodes[show->first]];
		char dir[LIB_PATH];
		copy(dir, sizeof dir, first->path);
		char *slash = strrchr(dir, '/');
		if (!slash)
			continue;
		*slash = 0;
		// Up out of "Season 1".
		slash = strrchr(dir, '/');
		if (slash && is_season_folder(slash + 1))
			*slash = 0;
		for (int pass = 0; pass < 2; pass++) {
			DIR *d = opendir(dir);
			if (!d)
				break;
			Entry *entries = malloc(256 * sizeof *entries);
			uint32_t count = 0;
			struct dirent *e;
			while (entries && (e = readdir(d)) && count < 256) {
				copy(entries[count].name, sizeof entries[count].name, e->d_name);
				entries[count].dir = e->d_type == DT_DIR;
				count++;
			}
			closedir(d);
			if (!entries)
				break;
			if (!show->poster[0])
				folder_image(entries, count, dir, SHOW_POSTER_NAMES, 4, show->poster, sizeof show->poster);
			if (!show->backdrop[0])
				folder_image(entries, count, dir, BACKDROP_NAMES, 3, show->backdrop, sizeof show->backdrop);
			if (!show->plot[0] && find_entry(entries, count, "tvshow.nfo")) {
				char nfo_path[LIB_PATH];
				join(nfo_path, sizeof nfo_path, dir, "tvshow.nfo");
				Nfo nfo;
				if (read_nfo(nfo_path, &nfo)) {
					if (nfo.title[0])
						copy(show->name, sizeof show->name, nfo.title);
					copy(show->plot, sizeof show->plot, nfo.plot);
					copy(show->genre, sizeof show->genre, nfo.genre);
					show->year = nfo.year;
				}
			}
			free(entries);
			// Episodes straight in the show folder: there's no level above to try.
			if (pass == 0 && show->poster[0])
				break;
			slash = strrchr(dir, '/');
			if (!slash || pass == 1)
				break;
			*slash = 0;
		}
		// No show poster: the first episode's own image.
		if (!show->poster[0])
			copy(show->poster, sizeof show->poster, first->poster);
		if (!show->backdrop[0])
			copy(show->backdrop, sizeof show->backdrop, first->backdrop);
	}
}

// --------------------------------------------------------------------------------------------------------------------
// user data

static UserRecord *find_user_record(const char *path, bool create)
{
	for (uint32_t i = 0; i < user_record_count; i++)
		if (!strcasecmp(user_records[i].path, path))
			return &user_records[i];
	if (!create)
		return NULL;
	if (user_record_count == user_record_capacity) {
		const uint32_t capacity = user_record_capacity ? user_record_capacity * 2 : 128;
		UserRecord *records = realloc(user_records, capacity * sizeof *records);
		if (!records)
			return NULL;
		user_records = records;
		user_record_capacity = capacity;
	}
	UserRecord *record = &user_records[user_record_count++];
	memset(record, 0, sizeof *record);
	copy(record->path, sizeof record->path, path);
	return record;
}

// Copies each item's user state in from the records.
static void attach_user(Library *lib)
{
	for (uint32_t i = 0; i < lib->count; i++) {
		const UserRecord *r = find_user_record(lib->items[i].path, false);
		if (r)
			lib->user[i] = r->user;
	}
}

static void user_path(char *out, size_t size)
{
	join(out, size, data_dir, "userdata.tsv");
}

static void load_user(void)
{
	char path[LIB_PATH];
	user_path(path, sizeof path);
	FILE *f = fopen(path, "rb");
	if (!f)
		return;
	char line[MAX_LINE];
	while (fgets(line, sizeof line, f)) {
		char *tab = strchr(line, '\t');
		if (!tab)
			continue;
		*tab = 0;
		unsigned position = 0, duration = 0, watched = 0, favorite = 0;
		long long last = 0;
		if (sscanf(tab + 1, "%u\t%u\t%u\t%u\t%lld", &position, &duration, &watched, &favorite, &last) < 4)
			continue;
		UserRecord *r = find_user_record(line, true);
		if (!r)
			break;
		r->user.position_s = position;
		r->user.duration_s = duration;
		r->user.watched = watched != 0;
		r->user.favorite = favorite != 0;
		r->user.last_played = last;
	}
	fclose(f);
}

void library_save_user(void)
{
	// The live items' state goes back into the records first.
	for (uint32_t i = 0; i < live.count; i++) {
		const MediaUser *u = &live.user[i];
		UserRecord *r = find_user_record(live.items[i].path, u->position_s || u->watched || u->favorite || u->last_played);
		if (r)
			r->user = *u;
	}
	char path[LIB_PATH];
	char temp[LIB_PATH + 8];
	user_path(path, sizeof path);
	snprintf(temp, sizeof temp, "%s.new", path);
	FILE *f = fopen(temp, "wb");
	if (!f) {
		smc_log("library: can't write %s: %d", temp, errno);
		return;
	}
	for (uint32_t i = 0; i < user_record_count; i++) {
		const MediaUser *u = &user_records[i].user;
		if (!u->position_s && !u->watched && !u->favorite && !u->last_played)
			continue;
		fprintf(f, "%s\t%u\t%u\t%u\t%u\t%lld\n", user_records[i].path, (unsigned)u->position_s, (unsigned)u->duration_s,
			u->watched ? 1u : 0u, u->favorite ? 1u : 0u, (long long)u->last_played);
	}
	fclose(f);
	// A file that's open can't be replaced, and there's no atomic replace over an existing one: remove, then rename.
	remove(path);
	if (rename(temp, path) != 0)
		smc_log("library: can't rename %s: %d", temp, errno);
}

void library_user_changed(void)
{
	library_save_user();
}

// --------------------------------------------------------------------------------------------------------------------
// the index file: one item a line, tab-separated, tabs and newlines in text turned to spaces

static void put_field(FILE *f, const char *s)
{
	for (; *s; s++)
		fputc(*s == '\t' || *s == '\n' || *s == '\r' ? ' ' : *s, f);
	fputc('\t', f);
}

static void save_index(const Library *lib)
{
	char path[LIB_PATH];
	char temp[LIB_PATH + 8];
	join(path, sizeof path, data_dir, "library.tsv");
	snprintf(temp, sizeof temp, "%s.new", path);
	FILE *f = fopen(temp, "wb");
	if (!f) {
		smc_log("library: can't write %s: %d", temp, errno);
		return;
	}
	fprintf(f, "SMCINDEX\t%d\t%u\n", INDEX_VERSION, (unsigned)lib->count);
	for (uint32_t i = 0; i < lib->count; i++) {
		const MediaItem *m = &lib->items[i];
		fprintf(f, "%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%lld\t", m->kind, m->season, m->episode, m->year, m->runtime_min,
			m->root, (unsigned long long)m->size_bytes, (long long)m->added);
		put_field(f, m->path);
		put_field(f, m->title);
		put_field(f, m->show);
		put_field(f, m->genre);
		put_field(f, m->poster);
		put_field(f, m->backdrop);
		put_field(f, m->container);
		put_field(f, m->plot);
		fputc('\n', f);
	}
	fclose(f);
	remove(path);
	if (rename(temp, path) != 0)
		smc_log("library: can't rename %s: %d", temp, errno);
}

static char *next_field(char **p)
{
	char *start = *p;
	char *tab = strchr(start, '\t');
	if (tab) {
		*tab = 0;
		*p = tab + 1;
	} else {
		*p = start + strlen(start);
	}
	return start;
}

static bool load_index(Library *lib)
{
	char path[LIB_PATH];
	join(path, sizeof path, data_dir, "library.tsv");
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	char *line = malloc(MAX_LINE);
	bool ok = false;
	int version = 0;
	unsigned count = 0;
	if (line && fgets(line, MAX_LINE, f) && sscanf(line, "SMCINDEX\t%d\t%u", &version, &count) == 2 &&
		version == INDEX_VERSION && count <= LIB_MAX_ITEMS) {
		lib->items = calloc(count + 1, sizeof *lib->items);
		lib->count = 0;
		while (lib->items && lib->count < count && fgets(line, MAX_LINE, f)) {
			char *nl = strchr(line, '\n');
			if (nl)
				*nl = 0;
			MediaItem *m = &lib->items[lib->count];
			unsigned kind, season, episode, year, runtime, root;
			unsigned long long size;
			long long added;
			if (sscanf(line, "%u\t%u\t%u\t%u\t%u\t%u\t%llu\t%lld\t", &kind, &season, &episode, &year, &runtime, &root,
					&size, &added) != 8)
				continue;
			char *p = line;
			for (int i = 0; i < 8; i++)
				next_field(&p);
			copy(m->path, sizeof m->path, next_field(&p));
			copy(m->title, sizeof m->title, next_field(&p));
			copy(m->show, sizeof m->show, next_field(&p));
			copy(m->genre, sizeof m->genre, next_field(&p));
			copy(m->poster, sizeof m->poster, next_field(&p));
			copy(m->backdrop, sizeof m->backdrop, next_field(&p));
			copy(m->container, sizeof m->container, next_field(&p));
			copy(m->plot, sizeof m->plot, next_field(&p));
			m->kind = (uint8_t)kind;
			m->season = (uint8_t)season;
			m->episode = (uint16_t)episode;
			m->year = (uint16_t)year;
			m->runtime_min = (uint16_t)runtime;
			m->root = (uint8_t)root;
			m->size_bytes = size;
			m->added = added;
			lib->count++;
		}
		ok = lib->items != NULL;
	}
	free(line);
	fclose(f);
	return ok;
}

// --------------------------------------------------------------------------------------------------------------------
// the public side

void library_init(const char *dir)
{
	copy(data_dir, sizeof data_dir, dir);
	free_library(&live);
	load_user();
	Library lib = { 0 };
	if (load_index(&lib) && build_views(&lib)) {
		show_details(&lib);
		attach_user(&lib);
		live = lib;
		smc_log("library: %u items from the saved index (%u movies, %u shows)", (unsigned)live.count,
			(unsigned)live.movie_count, (unsigned)live.show_count);
	} else {
		free_library(&lib);
	}
}

void library_set_roots(const LibraryRoot *r, uint32_t count)
{
	pthread_mutex_lock(&lock);
	root_count = count > LIB_MAX_ROOTS ? LIB_MAX_ROOTS : count;
	memcpy(roots, r, root_count * sizeof *roots);
	pthread_mutex_unlock(&lock);
}

void library_get_roots(LibraryRoot *r, uint32_t *count)
{
	pthread_mutex_lock(&lock);
	memcpy(r, roots, root_count * sizeof *roots);
	*count = root_count;
	pthread_mutex_unlock(&lock);
}

static void *scan_main(void *argument)
{
	(void)argument;
	Scan scan;
	memset(&scan, 0, sizeof scan);
	scan.start_ms = now_ms();
	LibraryRoot local[LIB_MAX_ROOTS];
	uint32_t count = 0;
	library_get_roots(local, &count);
	for (uint32_t r = 0; r < count; r++) {
		roots[r].found = 0;
		roots[r].present = false;
		if (!local[r].enabled)
			continue;
		struct stat st;
		const bool present = stat(local[r].path, &st) == 0 && S_ISDIR(st.st_mode);
		pthread_mutex_lock(&lock);
		roots[r].present = present;
		pthread_mutex_unlock(&lock);
		if (!present) {
			smc_log("library: %s isn't there (%d)", local[r].path, errno);
			continue;
		}
		scan.root = r;
		scan.show_name[0] = 0;
		scan_dir(&scan, local[r].path, 0, is_tv_root(local[r].path));
		smc_log("library: %s: %u videos", local[r].path, (unsigned)roots[r].found);
	}

	Library *lib = calloc(1, sizeof *lib);
	if (lib) {
		lib->items = scan.items;
		lib->count = scan.count;
		if (build_views(lib)) {
			show_details(lib);
			save_index(lib);
		} else {
			free_library(lib);
			free(lib);
			lib = NULL;
		}
	} else {
		free(scan.items);
	}
	const uint32_t ms = (uint32_t)(now_ms() - scan.start_ms);
	smc_log("library: scan done in %u ms: %u folders, %u files, %u videos, %u .nfo, %u posters", (unsigned)ms,
		(unsigned)scan.progress.folders, (unsigned)scan.progress.files, (unsigned)scan.progress.videos,
		(unsigned)scan.progress.nfos, (unsigned)scan.progress.posters);
	pthread_mutex_lock(&lock);
	if (pending) {
		free_library(pending);
		free(pending);
	}
	pending = lib;
	status = scan.progress;
	status.ms = ms;
	status.scanning = false;
	status.current[0] = 0;
	pthread_mutex_unlock(&lock);
	return NULL;
}

void library_scan_start(void)
{
	pthread_mutex_lock(&lock);
	const bool running = status.scanning;
	if (!running) {
		memset(&status, 0, sizeof status);
		status.scanning = true;
	}
	pthread_mutex_unlock(&lock);
	if (running)
		return;
	if (scan_thread_made) {
		pthread_join(scan_thread, NULL);
		scan_thread_made = false;
	}
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 256 * 1024);
	const int error = pthread_create(&scan_thread, &attr, scan_main, NULL);
	pthread_attr_destroy(&attr);
	if (error) {
		smc_log("library: can't start the scan thread: %d", error);
		pthread_mutex_lock(&lock);
		status.scanning = false;
		copy(status.error, sizeof status.error, "Couldn't start the scan");
		pthread_mutex_unlock(&lock);
		return;
	}
	scan_thread_made = true;
	threads_place(scan_thread, HW_IO);
}

void library_scan_status(ScanStatus *out)
{
	pthread_mutex_lock(&lock);
	*out = status;
	pthread_mutex_unlock(&lock);
}

bool library_pending(void)
{
	pthread_mutex_lock(&lock);
	const bool waiting = pending != NULL;
	pthread_mutex_unlock(&lock);
	return waiting;
}

bool library_poll(void)
{
	pthread_mutex_lock(&lock);
	Library *lib = pending;
	pending = NULL;
	pthread_mutex_unlock(&lock);
	if (!lib)
		return false;
	// Keep what the viewer did on the old items before they go.
	if (live.count)
		library_save_user();
	attach_user(lib);
	free_library(&live);
	live = *lib;
	free(lib);
	return true;
}

const Library *library_get(void)
{
	return &live;
}

MediaUser *library_user(uint32_t item)
{
	return item < live.count ? &live.user[item] : NULL;
}

// Sorts indices by a key, largest first; small lists, so insertion sort.
static void sort_desc(uint32_t *idx, int64_t *keys, uint32_t n)
{
	for (uint32_t i = 1; i < n; i++) {
		const uint32_t v = idx[i];
		const int64_t k = keys[i];
		uint32_t j = i;
		while (j && keys[j - 1] < k) {
			idx[j] = idx[j - 1];
			keys[j] = keys[j - 1];
			j--;
		}
		idx[j] = v;
		keys[j] = k;
	}
}

uint32_t library_continue_watching(uint32_t *out, uint32_t max)
{
	int64_t *keys = malloc((max + 1) * sizeof *keys);
	if (!keys)
		return 0;
	uint32_t n = 0;
	for (uint32_t i = 0; i < live.count; i++) {
		const MediaUser *u = &live.user[i];
		if (!u->position_s || u->watched)
			continue;
		if (n < max) {
			out[n] = i;
			keys[n++] = u->last_played;
		} else {
			// Replace the oldest if this is newer.
			uint32_t oldest = 0;
			for (uint32_t j = 1; j < n; j++)
				if (keys[j] < keys[oldest])
					oldest = j;
			if (u->last_played > keys[oldest]) {
				out[oldest] = i;
				keys[oldest] = u->last_played;
			}
		}
	}
	sort_desc(out, keys, n);
	free(keys);
	return n;
}

uint32_t library_recently_added(uint32_t *out, uint32_t max, bool shows_once)
{
	int64_t *keys = malloc((max + 1) * sizeof *keys);
	if (!keys)
		return 0;
	uint32_t n = 0;
	for (uint32_t i = 0; i < live.count; i++) {
		const MediaItem *m = &live.items[i];
		uint32_t candidate = i;
		if (shows_once && m->kind == MEDIA_EPISODE) {
			// The show's newest episode stands for it.
			const uint32_t s = library_show_of(i);
			if (s == UINT32_MAX || live.shows[s].added != m->added)
				continue;
			bool already = false;
			for (uint32_t j = 0; j < n; j++)
				if (live.items[out[j]].kind == MEDIA_EPISODE && library_show_of(out[j]) == s)
					already = true;
			if (already)
				continue;
		}
		if (n < max) {
			out[n] = candidate;
			keys[n++] = m->added;
		} else {
			uint32_t oldest = 0;
			for (uint32_t j = 1; j < n; j++)
				if (keys[j] < keys[oldest])
					oldest = j;
			if (m->added > keys[oldest]) {
				out[oldest] = candidate;
				keys[oldest] = m->added;
			}
		}
	}
	sort_desc(out, keys, n);
	free(keys);
	return n;
}

uint32_t library_favorites(uint32_t *out, uint32_t max)
{
	uint32_t n = 0;
	for (uint32_t i = 0; i < live.movie_count && n < max; i++)
		if (live.user[live.movies[i]].favorite)
			out[n++] = live.movies[i];
	for (uint32_t i = 0; i < live.episode_count && n < max; i++)
		if (live.user[live.episodes[i]].favorite)
			out[n++] = live.episodes[i];
	return n;
}

uint32_t library_search(const char *query, uint32_t *out, uint32_t max)
{
	uint32_t n = 0;
	if (!query[0])
		return 0;
	for (uint32_t i = 0; i < live.movie_count && n < max; i++) {
		const MediaItem *m = &live.items[live.movies[i]];
		if (strcasestr(m->title, query) || strcasestr(m->genre, query))
			out[n++] = live.movies[i];
	}
	for (uint32_t s = 0; s < live.show_count && n < max; s++) {
		const MediaShow *show = &live.shows[s];
		if (strcasestr(show->name, query) || strcasestr(show->genre, query)) {
			out[n++] = live.episodes[show->first];
			continue;
		}
		for (uint32_t e = 0; e < show->count && n < max; e++) {
			const uint32_t item = live.episodes[show->first + e];
			if (strcasestr(live.items[item].title, query))
				out[n++] = item;
		}
	}
	return n;
}

uint32_t library_show_of(uint32_t item)
{
	return item < live.count ? live.show_of[item] : UINT32_MAX;
}

uint32_t library_next_episode(uint32_t item)
{
	const uint32_t s = library_show_of(item);
	if (s == UINT32_MAX)
		return UINT32_MAX;
	const MediaShow *show = &live.shows[s];
	const uint32_t at = live.episode_at[item];
	return at + 1 < show->first + show->count ? live.episodes[at + 1] : UINT32_MAX;
}

void library_format_runtime(uint32_t minutes, char *out, uint32_t size)
{
	if (!minutes)
		out[0] = 0;
	else if (minutes < 60)
		snprintf(out, size, "%um", (unsigned)minutes);
	else
		snprintf(out, size, "%uh %um", (unsigned)(minutes / 60), (unsigned)(minutes % 60));
}
