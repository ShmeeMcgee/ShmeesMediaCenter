// Shmees Media Center: the UI. One thread draws and reads the controller at the display's rate; the library scan and
// the poster decoding run on threads of their own (library.c, art.c).

#define _GNU_SOURCE
#include "app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>

#include "art.h"
#include "gen/smc_assets.h"
#include "gfx.h"
#include "input.h"
#include "library.h"
#include "log.h"
#include "player.h"
#include "probe.h"
#include "settings.h"
#include "threads.h"
#include "xsdk/kernel.h"

#ifndef SMC_VERSION
#define SMC_VERSION "0.5.0-vector"
#endif

// --------------------------------------------------------------------------------------------------------------------
// the look

enum {
	C_BG = 0xFF101418,
	C_PANEL = 0xFF181E24,
	C_PANEL2 = 0xFF222A32,
	C_LINE = 0xFF2C353E,
	C_TEXT = 0xFFF2F5F7,
	C_DIM = 0xFF9AA4AE,
	C_FAINT = 0xFF5C6670,
	C_LIME = 0xFF96E63C,
	C_LIME_DIM = 0xFF5E9E1E,
	C_ON_LIME = 0xFF101418,
	C_RED = 0xFFE5484D,
	C_SHADOW = 0xA0000000,
};

enum {
	RAIL_W = 92,
	RAIL_OPEN_W = 320,
	CONTENT_X = 128,
	CONTENT_R = 1216,
	TOP_Y = 52,
	POSTER_W = 160,
	POSTER_H = 240,
	GRID_COLS = 6,
	GRID_GAP_X = 26,
	GRID_GAP_Y = 30,
	ROW_POSTER_W = 150,
	ROW_POSTER_H = 225,
	ROW_GAP = 22,
	MAX_CARDS = 6000,
	ROW_CARDS = 40,
	HOME_ROWS = 6,
	HERO_H = 410,
	STACK = 8,
	SPLASH_FRAMES = 120,
};

static uint32_t with_alpha(uint32_t argb, float a)
{
	if (a <= 0.0f)
		return argb & 0x00FFFFFF;
	if (a >= 1.0f)
		return argb;
	const uint32_t alpha = (uint32_t)((float)(argb >> 24) * a);
	return alpha << 24 | (argb & 0x00FFFFFF);
}

static float approach(float current, float target, float rate)
{
	const float d = target - current;
	if (fabsf(d) < 0.5f)
		return target;
	return current + d * rate;
}

// --------------------------------------------------------------------------------------------------------------------
// state

typedef enum {
	TAB_HOME,
	TAB_SEARCH,
	TAB_MOVIES,
	TAB_TV,
	TAB_RECENT,
	TAB_CONTINUE,
	TAB_FAVORITES,
	TAB_SETTINGS,
	TAB_COUNT,
} Tab;

static const char *const TAB_NAMES[TAB_COUNT] = { "Home", "Search", "Movies", "TV Shows", "Recently Added",
	"Continue Watching", "Favorites", "Settings" };
static const int TAB_ICONS[TAB_COUNT] = { ICON_HOME, ICON_SEARCH, ICON_FILM, ICON_TV, ICON_SPARKLE, ICON_CLOCK,
	ICON_HEART, ICON_GEAR };

typedef enum {
	VIEW_TAB,
	VIEW_DETAILS,
	VIEW_SHOW,
	VIEW_PLAYER,
	VIEW_DIAGNOSTICS,
} ViewKind;

typedef struct {
	ViewKind kind;
	uint32_t item;
	uint32_t show;
	int sel;
	int season;
	float scroll;
	float scroll_target;
	ProbeInfo probe;
	bool probed;
} View;

typedef enum {
	CARD_ITEM,
	CARD_SHOW,
} CardKind;

typedef struct {
	uint8_t kind;
	uint32_t index;
} Card;

typedef struct {
	int sel;
	float scroll;
	float scroll_target;
	int sort;
	int filter;
	Card *cards;
	uint32_t count;
	uint32_t version;
} Grid;

typedef struct {
	char title[32];
	Card cards[ROW_CARDS];
	uint32_t count;
	int col;
	float scroll;
} HomeRow;

static const char *const SORT_NAMES[] = { "A\xE2\x80\x93Z", "Newest", "Recently added" };
static const char *const FILTER_NAMES[] = { "All", "Unwatched", "Favorites" };

static struct {
	Settings settings;
	Input input;
	uint32_t frame;
	float time;
	bool quit;
	bool rail_focus;
	float rail_open;
	Tab tab;
	View stack[STACK];
	int depth;
	Grid grids[TAB_COUNT];
	// Home: the hero and the rows.
	HomeRow rows[HOME_ROWS];
	uint32_t row_count;
	uint32_t home_version;
	int home_row;
	float home_scroll;
	float home_scroll_target;
	Card hero[6];
	uint32_t hero_count;
	uint32_t hero_index;
	uint32_t hero_since;
	int hero_button;
	// Bumped whenever what the lists show could change.
	uint32_t data_version;
	// Search.
	char query[64];
	int key_row;
	int key_col;
	bool search_results_focus;
	// Settings.
	int settings_sel;
	float settings_scroll;
	// Dialog.
	bool confirm_exit;
	int confirm_sel;
	// Toast.
	char toast[96];
	uint32_t toast_until;
	// The player's overlay: frames since anything was pressed, the stats box, the end-of-episode countdown.
	uint32_t overlay_frames;
	bool player_stats;
	uint32_t ended_frame;
	uint32_t progress_saved_frame;
	// The decode benchmark on the diagnostics page.
	const struct BenchRun *bench_plan;
	uint32_t bench_items[8];
	uint32_t bench_count;
	uint32_t bench_next;
	PlayerBenchmark bench_results[8];
	int vector_probe;
	char vector_probe_text[160];
	uint32_t vector_compared;
	uint32_t vector_first_bad;
	bool bench_running;
	// Splash.
	bool splash;
	uint32_t splash_frame;
	uint32_t width;
	uint32_t height;
} ui;

static View *top(void)
{
	return &ui.stack[ui.depth - 1];
}

static void toast(const char *text)
{
	snprintf(ui.toast, sizeof ui.toast, "%s", text);
	ui.toast_until = ui.frame + 150;
}

static void push(View view)
{
	if (ui.depth == STACK)
		return;
	ui.stack[ui.depth++] = view;
}

static void pop(void)
{
	if (ui.depth > 1)
		ui.depth--;
}

static int64_t now_seconds(void)
{
	return (int64_t)time(NULL);
}

// What the VMX probe (a separate title, Hdd1:\Homebrew\ShmeesVMXProbe) found: the vector unit is offered only once it
// has run every instruction FFmpeg's PowerPC code uses.
enum {
	PROBE_NOT_RUN,
	PROBE_INCOMPLETE,
	PROBE_MISSING,
	PROBE_CLEAR,
};

static const char *const PROBE_RESULTS = "hdd:/Homebrew/ShmeesVMXProbe/vmxprobe-result.txt";

static int read_vector_probe(char *text, size_t size)
{
	FILE *f = fopen(PROBE_RESULTS, "rb");
	if (!f) {
		snprintf(text, size, "VMX probe not run yet");
		return PROBE_NOT_RUN;
	}
	char line[256];
	uint32_t ok = 0, missing = 0, untried = 0;
	char missing_names[96] = "";
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "ok", 2))
			ok++;
		else if (!strncmp(line, "MISSING", 7)) {
			missing++;
			char name[32] = "";
			if (sscanf(line + 7, "%31s", name) == 1 && strlen(missing_names) + strlen(name) + 2 < sizeof missing_names) {
				strcat(missing_names, name);
				strcat(missing_names, " ");
			}
		} else if (!strncmp(line, "untried", 7))
			untried++;
	}
	fclose(f);
	if (missing) {
		snprintf(text, size, "VMX probe: %u missing: %s", (unsigned)missing, missing_names);
		return PROBE_MISSING;
	}
	if (untried || !ok) {
		snprintf(text, size, "VMX probe didn't finish (%u of %u ran)", (unsigned)ok, (unsigned)(ok + untried));
		return PROBE_INCOMPLETE;
	}
	snprintf(text, size, "VMX probe: all %u instructions ran", (unsigned)ok);
	return PROBE_CLEAR;
}

// --------------------------------------------------------------------------------------------------------------------
// cards: what a grid or row shows

static const Library *lib(void)
{
	return library_get();
}

// An episode on a card is named by its show; the episode shows underneath ("S1 · E3").
static const char *card_title(const Card *c)
{
	if (c->kind == CARD_SHOW)
		return lib()->shows[c->index].name;
	const MediaItem *m = &lib()->items[c->index];
	return m->kind == MEDIA_EPISODE && m->show[0] ? m->show : m->title;
}

static const char *card_poster(const Card *c)
{
	if (c->kind == CARD_SHOW)
		return lib()->shows[c->index].poster;
	const MediaItem *m = &lib()->items[c->index];
	if (!m->poster[0] && m->kind == MEDIA_EPISODE) {
		const uint32_t s = library_show_of(c->index);
		if (s != UINT32_MAX)
			return lib()->shows[s].poster;
	}
	return m->poster;
}

static const char *card_backdrop(const Card *c)
{
	if (c->kind == CARD_SHOW)
		return lib()->shows[c->index].backdrop;
	const MediaItem *m = &lib()->items[c->index];
	if (!m->backdrop[0] && m->kind == MEDIA_EPISODE) {
		const uint32_t s = library_show_of(c->index);
		if (s != UINT32_MAX)
			return lib()->shows[s].backdrop;
	}
	return m->backdrop;
}

static float card_progress(const Card *c)
{
	if (c->kind != CARD_ITEM)
		return 0.0f;
	const MediaUser *u = &lib()->user[c->index];
	if (u->watched || !u->duration_s || !u->position_s)
		return 0.0f;
	return (float)u->position_s / (float)u->duration_s;
}

static bool card_watched(const Card *c)
{
	if (c->kind == CARD_SHOW) {
		const MediaShow *s = &lib()->shows[c->index];
		for (uint32_t e = 0; e < s->count; e++)
			if (!lib()->user[lib()->episodes[s->first + e]].watched)
				return false;
		return s->count > 0;
	}
	return lib()->user[c->index].watched;
}

static bool card_favorite(const Card *c)
{
	if (c->kind == CARD_SHOW) {
		const MediaShow *s = &lib()->shows[c->index];
		return s->count && lib()->user[lib()->episodes[s->first]].favorite;
	}
	return lib()->user[c->index].favorite;
}

static Card item_card(uint32_t item)
{
	return (Card){ .kind = CARD_ITEM, .index = item };
}

// An episode in a list of mixed things stands for its show, except where its own progress is the point.
static Card show_card_for(uint32_t item)
{
	const uint32_t s = library_show_of(item);
	if (s != UINT32_MAX)
		return (Card){ .kind = CARD_SHOW, .index = s };
	return item_card(item);
}

static const Library *sort_lib;
static int sort_mode;

static int compare_cards(const void *a, const void *b)
{
	const Card *x = a;
	const Card *y = b;
	if (sort_mode == 1) {
		const uint16_t yx = x->kind == CARD_SHOW ? sort_lib->shows[x->index].year : sort_lib->items[x->index].year;
		const uint16_t yy = y->kind == CARD_SHOW ? sort_lib->shows[y->index].year : sort_lib->items[y->index].year;
		if (yx != yy)
			return (int)yy - (int)yx;
	} else if (sort_mode == 2) {
		const int64_t ax = x->kind == CARD_SHOW ? sort_lib->shows[x->index].added : sort_lib->items[x->index].added;
		const int64_t ay = y->kind == CARD_SHOW ? sort_lib->shows[y->index].added : sort_lib->items[y->index].added;
		if (ax != ay)
			return ax < ay ? 1 : -1;
	}
	const char *tx = x->kind == CARD_SHOW ? sort_lib->shows[x->index].name : sort_lib->items[x->index].title;
	const char *ty = y->kind == CARD_SHOW ? sort_lib->shows[y->index].name : sort_lib->items[y->index].title;
	if (!strncasecmp(tx, "the ", 4))
		tx += 4;
	if (!strncasecmp(ty, "the ", 4))
		ty += 4;
	return strcasecmp(tx, ty);
}

static bool passes_filter(const Card *c, int filter)
{
	if (filter == 1)
		return !card_watched(c);
	if (filter == 2)
		return card_favorite(c);
	return true;
}

static uint32_t *scratch;

static void build_grid(Tab tab)
{
	Grid *g = &ui.grids[tab];
	if (g->version == ui.data_version && g->cards)
		return;
	g->version = ui.data_version;
	if (!g->cards)
		g->cards = malloc(MAX_CARDS * sizeof *g->cards);
	if (!scratch)
		scratch = malloc(MAX_CARDS * sizeof *scratch);
	g->count = 0;
	if (!g->cards || !scratch)
		return;
	const Library *l = lib();
	Card temp;
	switch (tab) {
	case TAB_MOVIES:
		for (uint32_t i = 0; i < l->movie_count; i++) {
			temp = item_card(l->movies[i]);
			if (passes_filter(&temp, g->filter))
				g->cards[g->count++] = temp;
		}
		break;
	case TAB_TV:
		for (uint32_t i = 0; i < l->show_count; i++) {
			temp = (Card){ .kind = CARD_SHOW, .index = i };
			if (passes_filter(&temp, g->filter))
				g->cards[g->count++] = temp;
		}
		break;
	case TAB_RECENT: {
		const uint32_t n = library_recently_added(scratch, 60, true);
		for (uint32_t i = 0; i < n; i++) {
			const MediaItem *m = &l->items[scratch[i]];
			temp = m->kind == MEDIA_EPISODE ? show_card_for(scratch[i]) : item_card(scratch[i]);
			if (passes_filter(&temp, g->filter))
				g->cards[g->count++] = temp;
		}
		// Already newest first.
		return;
	}
	case TAB_CONTINUE: {
		const uint32_t n = library_continue_watching(scratch, 60);
		for (uint32_t i = 0; i < n; i++)
			g->cards[g->count++] = item_card(scratch[i]);
		return;
	}
	case TAB_FAVORITES: {
		const uint32_t n = library_favorites(scratch, MAX_CARDS);
		// A favourite show is marked on its first episode; it shows as the show.
		for (uint32_t i = 0; i < n; i++) {
			const MediaItem *m = &l->items[scratch[i]];
			if (m->kind == MEDIA_EPISODE) {
				const uint32_t s = library_show_of(scratch[i]);
				if (s != UINT32_MAX && l->episodes[l->shows[s].first] == scratch[i]) {
					g->cards[g->count++] = (Card){ .kind = CARD_SHOW, .index = s };
					continue;
				}
			}
			g->cards[g->count++] = item_card(scratch[i]);
		}
		break;
	}
	case TAB_SEARCH: {
		const uint32_t n = library_search(ui.query, scratch, MAX_CARDS);
		for (uint32_t i = 0; i < n; i++) {
			const MediaItem *m = &l->items[scratch[i]];
			if (m->kind == MEDIA_EPISODE && strcasestr(m->show, ui.query) && !strcasestr(m->title, ui.query))
				g->cards[g->count++] = show_card_for(scratch[i]);
			else
				g->cards[g->count++] = item_card(scratch[i]);
		}
		return;
	}
	default:
		return;
	}
	sort_lib = l;
	sort_mode = g->sort;
	qsort(g->cards, g->count, sizeof *g->cards, compare_cards);
}

static void build_home(void)
{
	if (ui.home_version == ui.data_version)
		return;
	ui.home_version = ui.data_version;
	const Library *l = lib();
	if (!scratch)
		scratch = malloc(MAX_CARDS * sizeof *scratch);
	if (!scratch)
		return;
	ui.row_count = 0;
	HomeRow *row;
	uint32_t n;

	// The hero: what's being watched, then what's new.
	ui.hero_count = 0;
	n = library_continue_watching(scratch, 2);
	for (uint32_t i = 0; i < n && ui.hero_count < 6; i++)
		ui.hero[ui.hero_count++] = item_card(scratch[i]);
	n = library_recently_added(scratch, 8, true);
	for (uint32_t i = 0; i < n && ui.hero_count < 6; i++) {
		const Card c = l->items[scratch[i]].kind == MEDIA_EPISODE ? show_card_for(scratch[i]) : item_card(scratch[i]);
		bool dup = false;
		for (uint32_t j = 0; j < ui.hero_count; j++)
			if (ui.hero[j].kind == c.kind && ui.hero[j].index == c.index)
				dup = true;
		if (!dup)
			ui.hero[ui.hero_count++] = c;
	}
	if (ui.hero_index >= ui.hero_count)
		ui.hero_index = 0;

	n = library_continue_watching(scratch, ROW_CARDS);
	if (n) {
		row = &ui.rows[ui.row_count++];
		snprintf(row->title, sizeof row->title, "Continue Watching");
		row->count = 0;
		for (uint32_t i = 0; i < n; i++)
			row->cards[row->count++] = item_card(scratch[i]);
	}
	n = library_recently_added(scratch, ROW_CARDS, true);
	if (n) {
		row = &ui.rows[ui.row_count++];
		snprintf(row->title, sizeof row->title, "Recently Added");
		row->count = 0;
		for (uint32_t i = 0; i < n; i++)
			row->cards[row->count++] =
				l->items[scratch[i]].kind == MEDIA_EPISODE ? show_card_for(scratch[i]) : item_card(scratch[i]);
	}
	if (l->movie_count) {
		row = &ui.rows[ui.row_count++];
		snprintf(row->title, sizeof row->title, "Movies");
		row->count = 0;
		for (uint32_t i = 0; i < l->movie_count && row->count < ROW_CARDS; i++)
			row->cards[row->count++] = item_card(l->movies[i]);
	}
	if (l->show_count) {
		row = &ui.rows[ui.row_count++];
		snprintf(row->title, sizeof row->title, "TV Shows");
		row->count = 0;
		for (uint32_t i = 0; i < l->show_count && row->count < ROW_CARDS; i++)
			row->cards[row->count++] = (Card){ .kind = CARD_SHOW, .index = i };
	}
	n = library_favorites(scratch, ROW_CARDS);
	if (n) {
		row = &ui.rows[ui.row_count++];
		snprintf(row->title, sizeof row->title, "Favorites");
		row->count = 0;
		for (uint32_t i = 0; i < n; i++)
			row->cards[row->count++] = item_card(scratch[i]);
	}
	for (uint32_t r = 0; r < ui.row_count; r++)
		if (ui.rows[r].col >= (int)ui.rows[r].count)
			ui.rows[r].col = ui.rows[r].count ? (int)ui.rows[r].count - 1 : 0;
	const int max_row = (int)ui.row_count + (ui.hero_count ? 1 : 0) - 1;
	if (ui.home_row > max_row)
		ui.home_row = max_row < 0 ? 0 : max_row;
}

static void data_changed(void)
{
	ui.data_version++;
}

// --------------------------------------------------------------------------------------------------------------------
// drawing pieces

static void draw_badge(float x, float y, float size, float alpha)
{
	const GfxImage *badge = gfx_badge();
	if (badge && badge->valid)
		gfx_image(badge, x, y, size, size, with_alpha(0xFFFFFFFF, alpha));
	else {
		gfx_round_rect(x, y, size, size, size * 0.22f, with_alpha(C_LIME, alpha));
		gfx_text(FONT_TITLE, x + size * 0.32f, y + size * 0.2f, with_alpha(C_ON_LIME, alpha), "S");
	}
}

static void draw_meta_line(float x, float y, const char *const *parts, int n, uint32_t color)
{
	float px = x;
	for (int i = 0; i < n; i++) {
		if (!parts[i] || !parts[i][0])
			continue;
		if (px > x)
			px += gfx_text(FONT_BODY, px, y, C_FAINT, "  \xC2\xB7  ");
		px += gfx_text(FONT_BODY, px, y, color, parts[i]);
	}
}

// A poster, or a placeholder with the title on it, with focus, progress and watched marks.
static void draw_poster(const Card *c, float x, float y, float w, float h, bool focused, bool title_below)
{
	const float grow = focused ? 1.06f : 1.0f;
	const float gw = w * grow;
	const float gh = h * grow;
	const float gx = x - (gw - w) * 0.5f;
	const float gy = y - (gh - h) * 0.5f;
	gfx_shadow(gx, gy + 6.0f, gw, gh, focused ? 22.0f : 14.0f, focused ? 0xC0000000 : 0x80000000);
	const GfxImage *img = art_get(card_poster(c), ART_POSTER);
	if (img) {
		gfx_image_cover(img, gx, gy, gw, gh, 0xFFFFFFFF);
	} else {
		gfx_round_rect(gx, gy, gw, gh, 10.0f, C_PANEL2);
		gfx_gradient_v(gx + 4.0f, gy + gh * 0.45f, gw - 8.0f, gh * 0.55f - 4.0f, 0x00101418, 0x60101418);
		gfx_icon(c->kind == CARD_SHOW ? ICON_TV : ICON_FILM, gx + gw * 0.5f - 22.0f, gy + gh * 0.24f, 44.0f, C_FAINT);
		gfx_text_wrap(FONT_SMALL, gx + 12.0f, gy + gh * 0.52f, gw - 24.0f, 4, C_DIM, card_title(c));
	}
	const float p = card_progress(c);
	if (p > 0.0f) {
		gfx_rect(gx + 8.0f, gy + gh - 12.0f, gw - 16.0f, 5.0f, 0xA0000000);
		gfx_rect(gx + 8.0f, gy + gh - 12.0f, (gw - 16.0f) * (p > 1.0f ? 1.0f : p), 5.0f, C_LIME);
	}
	if (card_watched(c)) {
		gfx_circle(gx + gw - 18.0f, gy + 18.0f, 13.0f, C_LIME);
		gfx_icon(ICON_CHECK, gx + gw - 29.0f, gy + 7.0f, 22.0f, C_ON_LIME);
	}
	if (focused)
		gfx_round_outline(gx - 5.0f, gy - 5.0f, gw + 10.0f, gh + 10.0f, 14.0f, C_LIME);
	if (title_below) {
		gfx_text_fit(FONT_SMALL, x, y + h + 12.0f, w, focused ? C_TEXT : C_DIM, card_title(c));
		if (c->kind == CARD_SHOW) {
			char sub[48];
			const MediaShow *s = &lib()->shows[c->index];
			snprintf(sub, sizeof sub, "%u season%s", (unsigned)s->seasons, s->seasons == 1 ? "" : "s");
			gfx_text_fit(FONT_SMALL, x, y + h + 36.0f, w, C_FAINT, sub);
		} else if (lib()->items[c->index].kind == MEDIA_EPISODE) {
			char sub[48];
			const MediaItem *m = &lib()->items[c->index];
			snprintf(sub, sizeof sub, "S%u \xC2\xB7 E%u", (unsigned)m->season, (unsigned)m->episode);
			gfx_text_fit(FONT_SMALL, x, y + h + 36.0f, w, C_FAINT, sub);
		} else if (lib()->items[c->index].year) {
			char sub[16];
			snprintf(sub, sizeof sub, "%u", (unsigned)lib()->items[c->index].year);
			gfx_text_fit(FONT_SMALL, x, y + h + 36.0f, w, C_FAINT, sub);
		}
	}
}

static void draw_button(float x, float y, float w, int icon, const char *label, bool focused, bool primary)
{
	const float h = 54.0f;
	uint32_t fill = focused ? C_LIME : primary ? 0x40FFFFFF : 0x26FFFFFF;
	uint32_t ink = focused ? C_ON_LIME : C_TEXT;
	if (focused)
		gfx_shadow(x, y + 4.0f, w, h, 16.0f, 0x7096E63C);
	gfx_round_rect(x, y, w, h, 12.0f, fill);
	float tx = x + 20.0f;
	if (icon >= 0) {
		gfx_icon(icon, tx, y + 13.0f, 28.0f, ink);
		tx += 40.0f;
	}
	gfx_text(FONT_LABEL, tx, y + 12.0f, ink, label);
}

static float button_width(int icon, const char *label)
{
	return gfx_text_width(FONT_LABEL, label) + (icon >= 0 ? 80.0f : 40.0f);
}

// A full-screen backdrop behind a page, faded into the background towards the bottom and left. A poster stretched to
// full screen looks soft, so without a backdrop the page stays plain; returns whether one was drawn.
static bool draw_backdrop(const char *path, float alpha)
{
	const GfxImage *img = art_get(path, ART_BACKDROP);
	if (!img)
		return false;
	gfx_image_cover(img, 0.0f, 0.0f, (float)SCREEN_W, (float)SCREEN_H, with_alpha(0xFFFFFFFF, alpha * 0.5f));
	gfx_gradient_h(0.0f, 0.0f, 900.0f, (float)SCREEN_H, with_alpha(0xF8101418, alpha), 0x00101418);
	gfx_gradient_v(0.0f, 280.0f, (float)SCREEN_W, 440.0f, 0x00101418, with_alpha(0xFF101418, alpha));
	return true;
}

static void draw_hint(float x, float y, const char *button, uint32_t button_color, const char *label)
{
	gfx_circle(x + 13.0f, y + 13.0f, 13.0f, button_color);
	const float bw = gfx_text_width(FONT_SMALL, button);
	gfx_text(FONT_SMALL, x + 13.0f - bw * 0.5f, y + 2.0f, C_ON_LIME, button);
	gfx_text(FONT_SMALL, x + 34.0f, y + 2.0f, C_DIM, label);
}

static void draw_hints(const char *const *pairs, int n)
{
	// Right-aligned along the bottom: A, B, X, Y and their colours.
	float x = CONTENT_R;
	for (int i = n - 1; i >= 0; i--) {
		const char *b = pairs[i * 2];
		const char *label = pairs[i * 2 + 1];
		const uint32_t color = !strcmp(b, "A") ? 0xFF6CC24A : !strcmp(b, "B") ? 0xFFE5484D : !strcmp(b, "X") ? 0xFF3E8EDE : 0xFFF2C94C;
		x -= gfx_text_width(FONT_SMALL, label) + 34.0f;
		draw_hint(x, (float)SCREEN_H - 50.0f, b, color, label);
		x -= 26.0f;
	}
}

// --------------------------------------------------------------------------------------------------------------------
// the rail

static void draw_rail(void)
{
	const float open = ui.rail_open;
	const float w = RAIL_W + (RAIL_OPEN_W - RAIL_W) * open;
	if (open > 0.01f) {
		// Open, the rail covers what's behind it, and the page dims.
		gfx_rect(0.0f, 0.0f, SCREEN_W, SCREEN_H, with_alpha(0x90000000, open));
		gfx_rect(0.0f, 0.0f, w, SCREEN_H, with_alpha(0xF6101418, open));
		gfx_gradient_h(w, 0.0f, 120.0f, SCREEN_H, with_alpha(0xF6101418, open), 0x00101418);
	}
	gfx_gradient_h(0.0f, 0.0f, RAIL_W + 40.0f, (float)SCREEN_H, 0xE0101418, 0x00101418);
	draw_badge(24.0f, 30.0f, 44.0f, 1.0f);
	if (open > 0.05f) {
		gfx_text(FONT_LABEL, 82.0f, 32.0f, with_alpha(C_TEXT, open), "SHMEES");
		gfx_text(FONT_SMALL, 82.0f, 56.0f, with_alpha(C_LIME, open), "Media Center");
	}
	const float y0 = 150.0f;
	const float step = 58.0f;
	for (int t = 0; t < TAB_COUNT; t++) {
		const float y = y0 + step * (float)t;
		const bool current = ui.tab == (Tab)t;
		const bool focused = ui.rail_focus && current;
		if (focused)
			gfx_round_rect(14.0f, y - 8.0f, w - 28.0f, 50.0f, 12.0f, with_alpha(C_LIME, 0.95f));
		const uint32_t ink = focused ? C_ON_LIME : current ? C_LIME : C_DIM;
		gfx_icon(TAB_ICONS[t], 30.0f, y, 34.0f, ink);
		if (current && !focused)
			gfx_rect(4.0f, y + 2.0f, 4.0f, 30.0f, C_LIME);
		if (open > 0.05f)
			gfx_text(FONT_LABEL, 82.0f, y + 4.0f, with_alpha(ink, open), TAB_NAMES[t]);
	}
}

// --------------------------------------------------------------------------------------------------------------------
// opening things

static void open_card(const Card *c)
{
	View v;
	memset(&v, 0, sizeof v);
	if (c->kind == CARD_SHOW) {
		v.kind = VIEW_SHOW;
		v.show = c->index;
		// Start on the first episode not watched.
		const MediaShow *s = &lib()->shows[c->index];
		for (uint32_t e = 0; e < s->count; e++) {
			if (lib()->user[lib()->episodes[s->first + e]].watched)
				continue;
			// Which season tab that is: the seasons changed this many times before it.
			int index = 0;
			for (uint32_t k = 1; k <= e; k++)
				if (lib()->items[lib()->episodes[s->first + k]].season !=
					lib()->items[lib()->episodes[s->first + k - 1]].season)
					index++;
			v.season = index;
			// And the episode within it.
			uint32_t first_of_season = e;
			while (first_of_season > 0 && lib()->items[lib()->episodes[s->first + first_of_season - 1]].season ==
											  lib()->items[lib()->episodes[s->first + e]].season)
				first_of_season--;
			v.sel = (int)(e - first_of_season);
			break;
		}
	} else {
		v.kind = VIEW_DETAILS;
		v.item = c->index;
	}
	push(v);
}

static void start_playback(uint32_t item, bool from_start)
{
	View v;
	memset(&v, 0, sizeof v);
	v.kind = VIEW_PLAYER;
	v.item = item;
	v.sel = from_start ? 1 : 0;
	MediaUser *u = library_user(item);
	if (u) {
		u->last_played = now_seconds();
		library_user_changed();
		data_changed();
	}
	const double start = !from_start && u && u->position_s > 5 ? (double)u->position_s - 3.0 : 0.0;
	smc_log("player: %s (%s)", lib()->items[item].path, from_start ? "from the start" : "resume");
	ui.overlay_frames = 0;
	ui.ended_frame = 0;
	ui.progress_saved_frame = ui.frame;
	player_open(lib()->items[item].path, start, ui.settings.volume);
	push(v);
}

// --------------------------------------------------------------------------------------------------------------------
// home

static float home_row_top(int r)
{
	// Row 0 is the hero when there is one.
	const int has_hero = ui.hero_count ? 1 : 0;
	if (has_hero && r == 0)
		return 0.0f;
	const int index = r - has_hero;
	return (has_hero ? HERO_H + 20.0f : TOP_Y + 20.0f) + (float)index * (ROW_POSTER_H + 110.0f);
}

static void update_home(void)
{
	build_home();
	const int has_hero = ui.hero_count ? 1 : 0;
	const int rows = (int)ui.row_count + has_hero;
	if (!rows)
		return;
	const uint32_t p = ui.input.pressed;
	if (p & IN_UP && ui.home_row > 0)
		ui.home_row--;
	if (p & IN_DOWN && ui.home_row < rows - 1)
		ui.home_row++;
	if (has_hero && ui.home_row == 0) {
		if (p & IN_LEFT) {
			if (ui.hero_button > 0)
				ui.hero_button--;
			else
				ui.rail_focus = true;
		}
		if (p & IN_RIGHT && ui.hero_button < 1)
			ui.hero_button++;
		if (p & (IN_LB | IN_RB) && ui.hero_count > 1) {
			ui.hero_index = (ui.hero_index + (p & IN_RB ? 1 : ui.hero_count - 1)) % ui.hero_count;
			ui.hero_since = ui.frame;
		}
		if (p & IN_A && ui.hero_count) {
			const Card *c = &ui.hero[ui.hero_index];
			if (ui.hero_button == 0 && c->kind == CARD_ITEM)
				start_playback(c->index, false);
			else
				open_card(c);
		}
	} else {
		HomeRow *row = &ui.rows[ui.home_row - has_hero];
		if (p & IN_LEFT) {
			if (row->col > 0)
				row->col--;
			else
				ui.rail_focus = true;
		}
		if (p & IN_RIGHT && row->col < (int)row->count - 1)
			row->col++;
		if (p & IN_A && row->count)
			open_card(&row->cards[row->col]);
	}
	// The hero stays while its row is focused; the first row below it nudges up enough to show whole, and the rest
	// come up near the top.
	if (ui.home_row < has_hero)
		ui.home_scroll_target = 0.0f;
	else if (ui.home_row == has_hero)
		ui.home_scroll_target = has_hero ? 120.0f : 0.0f;
	else
		ui.home_scroll_target = home_row_top(ui.home_row) - 96.0f;
	if (ui.home_scroll_target < 0.0f)
		ui.home_scroll_target = 0.0f;
	// The hero turns over every 9 seconds while nobody's on it.
	if (ui.hero_count > 1 && (ui.home_row != 0 || ui.rail_focus) && ui.frame - ui.hero_since > 540) {
		ui.hero_index = (ui.hero_index + 1) % ui.hero_count;
		ui.hero_since = ui.frame;
	}
}

static void draw_empty_library(float x, float y)
{
	ScanStatus st;
	library_scan_status(&st);
	gfx_icon(ICON_FILM, x, y, 64.0f, C_LIME);
	gfx_text(FONT_TITLE, x, y + 84.0f, C_TEXT, st.scanning ? "Looking for your movies\xE2\x80\xA6" : "Your library is empty");
	if (st.scanning) {
		char line[160];
		snprintf(line, sizeof line, "%u folders, %u videos so far", (unsigned)st.folders, (unsigned)st.videos);
		gfx_text(FONT_BODY, x, y + 140.0f, C_DIM, line);
		gfx_text_fit(FONT_SMALL, x, y + 176.0f, 900.0f, C_FAINT, st.current);
		return;
	}
	gfx_text_wrap(FONT_BODY, x, y + 140.0f, 900.0f, 3, C_DIM,
		"Put movies in Hdd1:\\Movies or a USB drive's \\Movies folder, and shows in \\TV Shows (one folder per show). "
		"MP4 with H.264 video and AAC sound works best.");
	gfx_text_wrap(FONT_BODY, x, y + 250.0f, 900.0f, 2, C_DIM,
		"Posters: put Movie Name.jpg (or poster.jpg in the movie's folder) beside the file. Then choose Rescan in "
		"Settings.");
}

static void draw_home(void)
{
	const int has_hero = ui.hero_count ? 1 : 0;
	if (!ui.row_count && !has_hero) {
		draw_empty_library(CONTENT_X + 20.0f, 180.0f);
		return;
	}
	ui.home_scroll = approach(ui.home_scroll, ui.home_scroll_target, 0.2f);
	const float sy = ui.home_scroll;
	if (has_hero) {
		const Card *c = &ui.hero[ui.hero_index];
		const float fade = 1.0f - sy / 400.0f;
		if (fade > 0.0f) {
			if (!draw_backdrop(card_backdrop(c), fade)) {
				// No backdrop: the poster stands at the right instead.
				const GfxImage *poster = art_get(card_poster(c), ART_POSTER);
				if (poster) {
					const float pw = 240.0f, ph = 360.0f, px = 930.0f, py = 70.0f - sy;
					gfx_shadow(px, py + 10.0f, pw, ph, 30.0f, with_alpha(0xC0000000, fade));
					gfx_image_cover(poster, px, py, pw, ph, with_alpha(0xFFFFFFFF, fade));
				}
			}
			const float y = 96.0f - sy;
			const char *kicker = c->kind == CARD_ITEM && card_progress(c) > 0.0f ? "CONTINUE WATCHING"
								 : c->kind == CARD_SHOW											? "TV SHOW"
																								: "NEW IN YOUR LIBRARY";
			gfx_text(FONT_SMALL, CONTENT_X, y, with_alpha(C_LIME, fade), kicker);
			gfx_text_fit(FONT_HERO, CONTENT_X - 3.0f, y + 24.0f, 760.0f, with_alpha(C_TEXT, fade), card_title(c));
			char year[8] = "", runtime[16] = "", count[160] = "";
			const char *genre = "";
			const char *plot = "";
			if (c->kind == CARD_SHOW) {
				const MediaShow *s = &lib()->shows[c->index];
				if (s->year)
					snprintf(year, sizeof year, "%u", (unsigned)s->year);
				snprintf(count, sizeof count, "%u episodes", (unsigned)s->count);
				genre = s->genre;
				plot = s->plot;
			} else {
				const MediaItem *m = &lib()->items[c->index];
				if (m->year)
					snprintf(year, sizeof year, "%u", (unsigned)m->year);
				library_format_runtime(m->runtime_min, runtime, sizeof runtime);
				genre = m->genre;
				plot = m->plot;
				if (m->kind == MEDIA_EPISODE)
					snprintf(count, sizeof count, "S%u E%u  \xC2\xB7  %s", (unsigned)m->season, (unsigned)m->episode,
						m->title);
			}
			const char *parts[] = { count, year, runtime, genre };
			draw_meta_line(CONTENT_X, y + 100.0f, parts, 4, with_alpha(C_DIM, fade));
			if (plot[0])
				gfx_text_wrap(FONT_BODY, CONTENT_X, y + 138.0f, 640.0f, 2, with_alpha(C_TEXT, fade * 0.85f), plot);
			const bool here = ui.home_row == 0 && !ui.rail_focus;
			const bool resume = c->kind == CARD_ITEM && card_progress(c) > 0.0f;
			const char *play = c->kind == CARD_SHOW ? "Episodes" : resume ? "Resume" : "Play";
			const float bw = button_width(ICON_PLAY, play);
			draw_button(CONTENT_X, y + 214.0f, bw, c->kind == CARD_SHOW ? ICON_LIST : ICON_PLAY, play,
				here && ui.hero_button == 0, true);
			draw_button(CONTENT_X + bw + 16.0f, y + 214.0f, button_width(ICON_INFO, "Details"), ICON_INFO, "Details",
				here && ui.hero_button == 1, false);
			// Which of the featured this is; LB and RB step through them.
			for (uint32_t i = 0; i < ui.hero_count; i++)
				gfx_circle(CONTENT_X + 6.0f + (float)i * 18.0f, y + 292.0f, i == ui.hero_index ? 5.0f : 3.5f,
					with_alpha(i == ui.hero_index ? C_LIME : C_FAINT, fade));
		}
	}
	for (uint32_t r = 0; r < ui.row_count; r++) {
		HomeRow *row = &ui.rows[r];
		const int index = (int)r + has_hero;
		const float y = home_row_top(index) - sy;
		if (y > SCREEN_H || y + ROW_POSTER_H + 100.0f < 0.0f)
			continue;
		const bool row_focus = ui.home_row == index && !ui.rail_focus;
		gfx_text(FONT_LABEL, CONTENT_X, y, row_focus ? C_TEXT : C_DIM, row->title);
		// Keep the focused card in view, scrolling once it passes the fourth.
		const float step = ROW_POSTER_W + ROW_GAP;
		float target = (float)row->col * step - step * 3.0f;
		const float max_scroll = (float)row->count * step - (CONTENT_R - CONTENT_X);
		if (target > max_scroll)
			target = max_scroll;
		if (target < 0.0f)
			target = 0.0f;
		row->scroll = approach(row->scroll, target, 0.22f);
		for (uint32_t i = 0; i < row->count; i++) {
			const float x = CONTENT_X + (float)i * step - row->scroll;
			if (x > SCREEN_W || x + ROW_POSTER_W < 0.0f)
				continue;
			const bool focused = row_focus && (int)i == row->col;
			draw_poster(&row->cards[i], x, y + 44.0f, ROW_POSTER_W, ROW_POSTER_H, focused, ui.settings.show_titles);
		}
	}
}

// --------------------------------------------------------------------------------------------------------------------
// grids: Movies, TV Shows, Recently Added, Continue Watching, Favorites

static void update_grid(Tab tab)
{
	build_grid(tab);
	Grid *g = &ui.grids[tab];
	const uint32_t p = ui.input.pressed;
	const int n = (int)g->count;
	if (p & IN_Y && (tab == TAB_MOVIES || tab == TAB_TV)) {
		g->sort = (g->sort + 1) % 3;
		g->version = 0;
		data_changed();
		char line[64];
		snprintf(line, sizeof line, "Sorted: %s", SORT_NAMES[g->sort]);
		toast(line);
	}
	if (p & IN_X && (tab == TAB_MOVIES || tab == TAB_TV || tab == TAB_RECENT)) {
		g->filter = (g->filter + 1) % 3;
		g->sel = 0;
		data_changed();
		char line[64];
		snprintf(line, sizeof line, "Showing: %s", FILTER_NAMES[g->filter]);
		toast(line);
	}
	if (!n)
		return;
	if (g->sel >= n)
		g->sel = n - 1;
	if (p & IN_LEFT) {
		if (g->sel % GRID_COLS == 0)
			ui.rail_focus = true;
		else
			g->sel--;
	}
	if (p & IN_RIGHT && g->sel % GRID_COLS < GRID_COLS - 1 && g->sel < n - 1)
		g->sel++;
	if (p & IN_UP && g->sel >= GRID_COLS)
		g->sel -= GRID_COLS;
	if (p & IN_DOWN) {
		if (g->sel + GRID_COLS < n)
			g->sel += GRID_COLS;
		else if (g->sel / GRID_COLS < (n - 1) / GRID_COLS)
			g->sel = n - 1;
	}
	if (p & IN_LT)
		g->sel = g->sel >= GRID_COLS * 3 ? g->sel - GRID_COLS * 3 : 0;
	if (p & IN_RT)
		g->sel = g->sel + GRID_COLS * 3 < n ? g->sel + GRID_COLS * 3 : n - 1;
	if (p & IN_A)
		open_card(&g->cards[g->sel]);
}

static void draw_grid_cards(Grid *g, float x0, float y0, int cols, float pw, float ph, float gap_x, float bottom,
	bool focus)
{
	const float row_h = ph + (ui.settings.show_titles ? 66.0f : 0.0f) + GRID_GAP_Y;
	const int row = g->sel / cols;
	float target = (float)row * row_h - row_h * 0.5f;
	if (target < 0.0f)
		target = 0.0f;
	g->scroll_target = target;
	g->scroll = approach(g->scroll, g->scroll_target, 0.2f);
	gfx_clip(x0 - 20.0f, y0 - 20.0f, (float)SCREEN_W - x0 + 20.0f, bottom - y0 + 20.0f);
	for (uint32_t i = 0; i < g->count; i++) {
		const float x = x0 + (float)(i % (uint32_t)cols) * (pw + gap_x);
		const float y = y0 + (float)(i / (uint32_t)cols) * row_h - g->scroll;
		if (y > bottom || y + row_h < y0 - 40.0f)
			continue;
		draw_poster(&g->cards[i], x, y, pw, ph, focus && (int)i == g->sel, ui.settings.show_titles);
	}
	gfx_unclip();
}

static void draw_grid(Tab tab)
{
	build_grid(tab);
	Grid *g = &ui.grids[tab];
	gfx_text(FONT_TITLE, CONTENT_X, TOP_Y, C_TEXT, TAB_NAMES[tab]);
	char line[96];
	float x = CONTENT_X + gfx_text_width(FONT_TITLE, TAB_NAMES[tab]) + 20.0f;
	snprintf(line, sizeof line, "%u", (unsigned)g->count);
	gfx_text(FONT_BODY, x, TOP_Y + 10.0f, C_FAINT, line);
	if (tab == TAB_MOVIES || tab == TAB_TV || tab == TAB_RECENT) {
		if (tab != TAB_RECENT)
			snprintf(line, sizeof line, "%s  \xC2\xB7  %s", SORT_NAMES[g->sort], FILTER_NAMES[g->filter]);
		else
			snprintf(line, sizeof line, "%s", FILTER_NAMES[g->filter]);
		gfx_text(FONT_SMALL, CONTENT_R - gfx_text_width(FONT_SMALL, line), TOP_Y + 14.0f, C_DIM, line);
	}
	if (!g->count) {
		const char *empty = tab == TAB_CONTINUE	   ? "Nothing in progress. What you start watching shows up here."
							: tab == TAB_FAVORITES ? "No favorites yet. Open a movie and choose Add to Favorites."
												   : NULL;
		if (empty)
			gfx_text(FONT_BODY, CONTENT_X, TOP_Y + 80.0f, C_DIM, empty);
		else if (!lib()->count)
			draw_empty_library(CONTENT_X + 20.0f, 180.0f);
		else
			gfx_text(FONT_BODY, CONTENT_X, TOP_Y + 80.0f, C_DIM, "Nothing matches this filter. Press X to change it.");
		return;
	}
	draw_grid_cards(g, CONTENT_X + 6.0f, TOP_Y + 80.0f, GRID_COLS, POSTER_W, POSTER_H, GRID_GAP_X, SCREEN_H - 70.0f,
		!ui.rail_focus);
	if (tab == TAB_MOVIES || tab == TAB_TV) {
		const char *hints[] = { "X", "Filter", "Y", "Sort", "A", "Open" };
		draw_hints(hints, 3);
	}
}

// --------------------------------------------------------------------------------------------------------------------
// search

static const char *const KEYS[] = { "ABCDEFG", "HIJKLMN", "OPQRSTU", "VWXYZ-'", "1234567", "890&:.!" };
enum {
	KEY_ROWS = 6,
	KEY_COLS = 7,
	// The row of actions under the letters: space, delete, clear.
	ACTION_ROW = KEY_ROWS,
	SEARCH_COLS = 4,
};

static void query_changed(void)
{
	ui.grids[TAB_SEARCH].sel = 0;
	ui.grids[TAB_SEARCH].version = 0;
	data_changed();
}

static void update_search(void)
{
	const uint32_t p = ui.input.pressed;
	Grid *g = &ui.grids[TAB_SEARCH];
	build_grid(TAB_SEARCH);
	const size_t len = strlen(ui.query);
	if (p & IN_X && len) {
		ui.query[len - 1] = 0;
		query_changed();
	}
	if (p & IN_Y && len + 1 < sizeof ui.query) {
		ui.query[len] = ' ';
		ui.query[len + 1] = 0;
		query_changed();
	}
	if (ui.search_results_focus) {
		const int n = (int)g->count;
		if (!n) {
			ui.search_results_focus = false;
			return;
		}
		if (p & IN_LEFT) {
			if (g->sel % SEARCH_COLS == 0)
				ui.search_results_focus = false;
			else
				g->sel--;
		}
		if (p & IN_RIGHT && g->sel % SEARCH_COLS < SEARCH_COLS - 1 && g->sel < n - 1)
			g->sel++;
		if (p & IN_UP && g->sel >= SEARCH_COLS)
			g->sel -= SEARCH_COLS;
		if (p & IN_DOWN && g->sel + SEARCH_COLS < n)
			g->sel += SEARCH_COLS;
		if (p & IN_A)
			open_card(&g->cards[g->sel]);
		return;
	}
	const int cols = ui.key_row == ACTION_ROW ? 3 : KEY_COLS;
	if (p & IN_LEFT) {
		if (ui.key_col == 0)
			ui.rail_focus = true;
		else
			ui.key_col--;
	}
	if (p & IN_RIGHT) {
		if (ui.key_col < cols - 1)
			ui.key_col++;
		else if (g->count)
			ui.search_results_focus = true;
	}
	if (p & IN_UP && ui.key_row > 0) {
		if (ui.key_row == ACTION_ROW)
			ui.key_col = ui.key_col * 3;
		ui.key_row--;
	}
	if (p & IN_DOWN && ui.key_row < ACTION_ROW) {
		ui.key_row++;
		if (ui.key_row == ACTION_ROW)
			ui.key_col = ui.key_col / 3 > 2 ? 2 : ui.key_col / 3;
	}
	if (p & IN_A) {
		if (ui.key_row == ACTION_ROW) {
			if (ui.key_col == 0 && len + 1 < sizeof ui.query) {
				ui.query[len] = ' ';
				ui.query[len + 1] = 0;
			} else if (ui.key_col == 1 && len)
				ui.query[len - 1] = 0;
			else if (ui.key_col == 2)
				ui.query[0] = 0;
			query_changed();
		} else if (len + 1 < sizeof ui.query) {
			ui.query[len] = KEYS[ui.key_row][ui.key_col];
			ui.query[len + 1] = 0;
			query_changed();
		}
	}
}

static void draw_search(void)
{
	build_grid(TAB_SEARCH);
	Grid *g = &ui.grids[TAB_SEARCH];
	gfx_text(FONT_TITLE, CONTENT_X, TOP_Y, C_TEXT, "Search");
	// The query box.
	const float bx = CONTENT_X;
	const float by = TOP_Y + 64.0f;
	gfx_round_rect(bx, by, 420.0f, 58.0f, 12.0f, C_PANEL2);
	gfx_icon(ICON_SEARCH, bx + 16.0f, by + 15.0f, 28.0f, C_DIM);
	const float tw = gfx_text_fit(FONT_LABEL, bx + 56.0f, by + 15.0f, 340.0f, ui.query[0] ? C_TEXT : C_FAINT,
		ui.query[0] ? ui.query : "Titles, shows, genres");
	if (ui.query[0] && (ui.frame / 30) % 2 == 0)
		gfx_rect(bx + 58.0f + tw, by + 14.0f, 2.0f, 30.0f, C_LIME);
	// The keyboard.
	const float kx = CONTENT_X;
	const float ky = by + 84.0f;
	const float key = 54.0f;
	const float gap = 6.0f;
	const bool kb_focus = !ui.rail_focus && !ui.search_results_focus;
	for (int r = 0; r < KEY_ROWS; r++)
		for (int c = 0; c < KEY_COLS; c++) {
			const float x = kx + (float)c * (key + gap);
			const float y = ky + (float)r * (key + gap);
			const bool focused = kb_focus && ui.key_row == r && ui.key_col == c;
			gfx_round_rect(x, y, key, key, 10.0f, focused ? C_LIME : C_PANEL);
			char s[2] = { KEYS[r][c], 0 };
			const float w = gfx_text_width(FONT_LABEL, s);
			gfx_text(FONT_LABEL, x + (key - w) * 0.5f, y + 13.0f, focused ? C_ON_LIME : C_TEXT, s);
		}
	static const char *const actions[] = { "Space", "Delete", "Clear" };
	const float aw = (KEY_COLS * (key + gap) - gap - 2.0f * gap) / 3.0f;
	for (int c = 0; c < 3; c++) {
		const float x = kx + (float)c * (aw + gap);
		const float y = ky + (float)KEY_ROWS * (key + gap);
		const bool focused = kb_focus && ui.key_row == ACTION_ROW && ui.key_col == c;
		gfx_round_rect(x, y, aw, key, 10.0f, focused ? C_LIME : C_PANEL);
		const float w = gfx_text_width(FONT_SMALL, actions[c]);
		gfx_text(FONT_SMALL, x + (aw - w) * 0.5f, y + 16.0f, focused ? C_ON_LIME : C_DIM, actions[c]);
	}
	// The results.
	const float rx = 580.0f;
	if (!ui.query[0]) {
		gfx_text_wrap(FONT_BODY, rx, TOP_Y + 80.0f, 600.0f, 3, C_DIM,
			"Type with the keyboard: X deletes, Y adds a space. Results show here as you type.");
	} else if (!g->count) {
		gfx_text(FONT_BODY, rx, TOP_Y + 80.0f, C_DIM, "No matches");
	} else {
		char line[64];
		snprintf(line, sizeof line, "%u result%s", (unsigned)g->count, g->count == 1 ? "" : "s");
		gfx_text(FONT_SMALL, rx, TOP_Y + 20.0f, C_DIM, line);
		draw_grid_cards(g, rx, TOP_Y + 64.0f, SEARCH_COLS, 130.0f, 195.0f, 22.0f, SCREEN_H - 60.0f,
			!ui.rail_focus && ui.search_results_focus);
	}
}

// --------------------------------------------------------------------------------------------------------------------
// settings

typedef enum {
	SET_ANIMATION,
	SET_SCAN_START,
	SET_AUTOPLAY,
	SET_TITLES,
	SET_VECTOR,
	SET_FOLDER0,
	SET_RESCAN = SET_FOLDER0 + LIB_MAX_ROOTS,
	SET_DIAGNOSTICS,
	SET_EXIT,
	SET_COUNT,
} SettingRow;

static bool setting_visible(int row)
{
	if (row >= SET_FOLDER0 && row < SET_RESCAN)
		return (uint32_t)(row - SET_FOLDER0) < ui.settings.root_count;
	return true;
}

static void apply_roots(void)
{
	library_set_roots(ui.settings.roots, ui.settings.root_count);
}

static void update_settings(void)
{
	const uint32_t p = ui.input.pressed;
	if (p & IN_LEFT)
		ui.rail_focus = true;
	if (p & IN_UP) {
		int r = ui.settings_sel - 1;
		while (r >= 0 && !setting_visible(r))
			r--;
		if (r >= 0)
			ui.settings_sel = r;
	}
	if (p & IN_DOWN) {
		int r = ui.settings_sel + 1;
		while (r < SET_COUNT && !setting_visible(r))
			r++;
		if (r < SET_COUNT)
			ui.settings_sel = r;
	}
	if (!(p & IN_A))
		return;
	Settings *s = &ui.settings;
	switch (ui.settings_sel) {
	case SET_ANIMATION:
		s->startup_animation = !s->startup_animation;
		break;
	case SET_SCAN_START:
		s->scan_on_start = !s->scan_on_start;
		break;
	case SET_AUTOPLAY:
		s->autoplay_next = !s->autoplay_next;
		break;
	case SET_TITLES:
		s->show_titles = !s->show_titles;
		break;
	case SET_VECTOR:
		if (!s->vector && ui.vector_probe != PROBE_CLEAR) {
			toast("The VMX probe has to clear the vector unit first");
			return;
		}
		s->vector = !s->vector;
		player_set_vector(s->vector);
		toast(s->vector ? "Vector unit on for the next video" : "Vector unit off");
		break;
	case SET_RESCAN:
		apply_roots();
		library_scan_start();
		toast("Rescanning your library");
		return;
	case SET_DIAGNOSTICS: {
		View v;
		memset(&v, 0, sizeof v);
		v.kind = VIEW_DIAGNOSTICS;
		push(v);
		return;
	}
	case SET_EXIT:
		ui.confirm_exit = true;
		ui.confirm_sel = 1;
		return;
	default:
		if (ui.settings_sel >= SET_FOLDER0 && ui.settings_sel < SET_RESCAN) {
			LibraryRoot *r = &s->roots[ui.settings_sel - SET_FOLDER0];
			r->enabled = !r->enabled;
			apply_roots();
			toast("Folders changed: choose Rescan library to update");
		}
		break;
	}
	settings_save(s);
}

static void draw_toggle(float x, float y, bool on)
{
	gfx_round_rect(x, y, 58.0f, 32.0f, 16.0f, on ? C_LIME : C_LINE);
	gfx_circle(on ? x + 42.0f : x + 16.0f, y + 16.0f, 12.0f, on ? C_ON_LIME : C_DIM);
}

static void draw_settings(void)
{
	gfx_text(FONT_TITLE, CONTENT_X, TOP_Y, C_TEXT, "Settings");
	LibraryRoot roots[LIB_MAX_ROOTS];
	uint32_t root_count = 0;
	library_get_roots(roots, &root_count);
	ScanStatus st;
	library_scan_status(&st);

	const float row_h = 64.0f;
	float y = TOP_Y + 74.0f;
	// Scroll so the selected row stays on screen.
	float sel_y = 0.0f;
	{
		float yy = 0.0f;
		for (int r = 0; r < SET_COUNT; r++) {
			if (!setting_visible(r))
				continue;
			if (r == SET_FOLDER0 || r == SET_RESCAN)
				yy += 44.0f;
			if (r == ui.settings_sel)
				sel_y = yy;
			yy += row_h;
		}
	}
	const float target = sel_y > 380.0f ? sel_y - 380.0f : 0.0f;
	ui.settings_scroll = approach(ui.settings_scroll, target, 0.2f);
	y -= ui.settings_scroll;
	gfx_clip(CONTENT_X - 10.0f, TOP_Y + 60.0f, CONTENT_R - CONTENT_X + 20.0f, SCREEN_H - TOP_Y - 120.0f);
	const float w = 760.0f;
	for (int r = 0; r < SET_COUNT; r++) {
		if (!setting_visible(r))
			continue;
		if (r == SET_FOLDER0) {
			gfx_text(FONT_SMALL, CONTENT_X, y + 14.0f, C_LIME, "LIBRARY FOLDERS");
			y += 44.0f;
		}
		if (r == SET_RESCAN) {
			gfx_text(FONT_SMALL, CONTENT_X, y + 14.0f, C_LIME, "LIBRARY");
			y += 44.0f;
		}
		const bool focused = !ui.rail_focus && ui.settings_sel == r;
		gfx_round_rect(CONTENT_X, y, w, row_h - 8.0f, 12.0f, focused ? C_PANEL2 : C_PANEL);
		if (focused)
			gfx_round_outline(CONTENT_X - 2.0f, y - 2.0f, w + 4.0f, row_h - 4.0f, 13.0f, C_LIME);
		const float ty = y + 15.0f;
		const float tx = CONTENT_X + 22.0f;
		char line[160];
		switch (r) {
		case SET_ANIMATION:
			gfx_text(FONT_LABEL, tx, ty, C_TEXT, "Startup animation");
			draw_toggle(CONTENT_X + w - 80.0f, y + 12.0f, ui.settings.startup_animation);
			break;
		case SET_SCAN_START:
			gfx_text(FONT_LABEL, tx, ty, C_TEXT, "Scan for new videos at startup");
			draw_toggle(CONTENT_X + w - 80.0f, y + 12.0f, ui.settings.scan_on_start);
			break;
		case SET_AUTOPLAY:
			gfx_text(FONT_LABEL, tx, ty, C_TEXT, "Play the next episode automatically");
			draw_toggle(CONTENT_X + w - 80.0f, y + 12.0f, ui.settings.autoplay_next);
			break;
		case SET_TITLES:
			gfx_text(FONT_LABEL, tx, ty, C_TEXT, "Titles under posters");
			draw_toggle(CONTENT_X + w - 80.0f, y + 12.0f, ui.settings.show_titles);
			break;
		case SET_VECTOR:
			gfx_text(FONT_LABEL, tx, ty, ui.vector_probe == PROBE_CLEAR ? C_TEXT : C_DIM, "Vector unit for video");
			{
				const char *sub = ui.vector_probe == PROBE_CLEAR ? "Experimental" : "Needs the VMX probe";
				gfx_text(FONT_SMALL, CONTENT_X + w - 100.0f - gfx_text_width(FONT_SMALL, sub), ty + 3.0f, C_FAINT, sub);
			}
			draw_toggle(CONTENT_X + w - 80.0f, y + 12.0f, ui.settings.vector);
			break;
		case SET_RESCAN:
			gfx_icon(ICON_SPARKLE, tx, ty - 2.0f, 30.0f, C_LIME);
			if (st.scanning)
				snprintf(line, sizeof line, "Scanning\xE2\x80\xA6 %u videos so far", (unsigned)st.videos);
			else
				snprintf(line, sizeof line, "Rescan library");
			gfx_text(FONT_LABEL, tx + 44.0f, ty, C_TEXT, line);
			if (!st.scanning && st.ms) {
				snprintf(line, sizeof line, "Last: %u videos in %.1f s", (unsigned)st.videos, (double)st.ms / 1000.0);
				gfx_text(FONT_SMALL, CONTENT_X + w - 20.0f - gfx_text_width(FONT_SMALL, line), ty + 3.0f, C_DIM, line);
			}
			break;
		case SET_DIAGNOSTICS:
			gfx_icon(ICON_INFO, tx, ty - 2.0f, 30.0f, C_DIM);
			gfx_text(FONT_LABEL, tx + 44.0f, ty, C_TEXT, "Diagnostics");
			gfx_text(FONT_SMALL, CONTENT_X + w - 20.0f - gfx_text_width(FONT_SMALL, "Drives, memory, video, version"),
				ty + 3.0f, C_DIM, "Drives, memory, video, version");
			break;
		case SET_EXIT:
			gfx_icon(ICON_BACK, tx, ty - 2.0f, 30.0f, C_DIM);
			gfx_text(FONT_LABEL, tx + 44.0f, ty, C_TEXT, "Exit to Aurora");
			break;
		default: {
			const uint32_t i = (uint32_t)(r - SET_FOLDER0);
			const LibraryRoot *root = &ui.settings.roots[i];
			const bool usb = !strncasecmp(root->path, "usb", 3);
			gfx_icon(usb ? ICON_USB : ICON_DRIVE, tx, ty - 2.0f, 30.0f, root->enabled ? C_LIME : C_FAINT);
			gfx_text(FONT_LABEL, tx + 44.0f, ty, root->enabled ? C_TEXT : C_DIM, root->path);
			if (i < root_count && root->enabled) {
				if (roots[i].present)
					snprintf(line, sizeof line, "%u video%s", (unsigned)roots[i].found, roots[i].found == 1 ? "" : "s");
				else
					snprintf(line, sizeof line, "%s", st.scanning ? "" : "Not found");
				gfx_text(FONT_SMALL, CONTENT_X + w - 110.0f - gfx_text_width(FONT_SMALL, line), ty + 3.0f, C_DIM, line);
			}
			draw_toggle(CONTENT_X + w - 80.0f, y + 12.0f, root->enabled);
			break;
		}
		}
		y += row_h;
	}
	gfx_unclip();
	// What the folders mean.
	gfx_text_wrap(FONT_SMALL, CONTENT_X + w + 40.0f, TOP_Y + 80.0f, CONTENT_R - CONTENT_X - w - 40.0f, 12, C_FAINT,
		"hdd: is the hard drive (Hdd1), usb0: the first USB drive. Folder names with TV, Show or Series in them are "
		"read as one folder per show. Edit game:\\data\\settings.txt to add other folders.");
}

// --------------------------------------------------------------------------------------------------------------------
// details

static void toggle_favorite(uint32_t item)
{
	MediaUser *u = library_user(item);
	if (!u)
		return;
	u->favorite = !u->favorite;
	library_user_changed();
	data_changed();
	toast(u->favorite ? "Added to Favorites" : "Removed from Favorites");
}

static void toggle_watched(uint32_t item)
{
	MediaUser *u = library_user(item);
	if (!u)
		return;
	u->watched = !u->watched;
	if (u->watched)
		u->position_s = 0;
	library_user_changed();
	data_changed();
	toast(u->watched ? "Marked as watched" : "Marked as unwatched");
}

typedef enum {
	ACT_RESUME,
	ACT_PLAY,
	ACT_NEXT,
	ACT_FAVORITE,
	ACT_WATCHED,
	ACT_SHOW,
} DetailAction;

static int detail_actions(uint32_t item, DetailAction *out)
{
	int n = 0;
	const MediaUser *u = &lib()->user[item];
	if (u->position_s && !u->watched)
		out[n++] = ACT_RESUME;
	out[n++] = ACT_PLAY;
	if (lib()->items[item].kind == MEDIA_EPISODE && library_next_episode(item) != UINT32_MAX)
		out[n++] = ACT_NEXT;
	out[n++] = ACT_FAVORITE;
	out[n++] = ACT_WATCHED;
	if (lib()->items[item].kind == MEDIA_EPISODE)
		out[n++] = ACT_SHOW;
	return n;
}

static void update_details(View *v)
{
	DetailAction acts[8];
	const int n = detail_actions(v->item, acts);
	const uint32_t p = ui.input.pressed;
	if (v->sel >= n)
		v->sel = n - 1;
	if (p & IN_LEFT && v->sel > 0)
		v->sel--;
	if (p & IN_RIGHT && v->sel < n - 1)
		v->sel++;
	if (p & IN_Y)
		toggle_favorite(v->item);
	if (!(p & IN_A))
		return;
	switch (acts[v->sel]) {
	case ACT_RESUME:
		start_playback(v->item, false);
		break;
	case ACT_PLAY:
		start_playback(v->item, true);
		break;
	case ACT_NEXT: {
		const uint32_t next = library_next_episode(v->item);
		if (next != UINT32_MAX) {
			v->item = next;
			v->probed = false;
			v->sel = 0;
		}
		break;
	}
	case ACT_FAVORITE:
		toggle_favorite(v->item);
		break;
	case ACT_WATCHED:
		toggle_watched(v->item);
		break;
	case ACT_SHOW: {
		const uint32_t s = library_show_of(v->item);
		if (s != UINT32_MAX) {
			const Card c = { .kind = CARD_SHOW, .index = s };
			open_card(&c);
		}
		break;
	}
	}
}

static void format_size(uint64_t bytes, char *out, size_t size)
{
	if (bytes >= (1ull << 30))
		snprintf(out, size, "%.2f GB", (double)bytes / (double)(1ull << 30));
	else
		snprintf(out, size, "%.0f MB", (double)bytes / (double)(1ull << 20));
}

static void format_clock(uint32_t seconds, char *out, size_t size)
{
	if (seconds >= 3600)
		snprintf(out, size, "%u:%02u:%02u", (unsigned)(seconds / 3600), (unsigned)(seconds / 60 % 60),
			(unsigned)(seconds % 60));
	else
		snprintf(out, size, "%u:%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60));
}

static void draw_details(View *v)
{
	const MediaItem *m = &lib()->items[v->item];
	const MediaUser *u = &lib()->user[v->item];
	const Card c = item_card(v->item);
	if (!v->probed) {
		// One read of the file's index, on the UI thread: a few small reads.
		probe_file(m->path, &v->probe);
		v->probed = true;
		if (v->probe.duration_s && !m->runtime_min) {
			MediaUser *mu = library_user(v->item);
			if (mu && !mu->duration_s)
				mu->duration_s = v->probe.duration_s;
		}
	}
	draw_backdrop(card_backdrop(&c), 1.0f);
	const float px = CONTENT_X;
	const float py = 110.0f;
	const float pw = 250.0f;
	const float ph = 375.0f;
	gfx_shadow(px, py + 8.0f, pw, ph, 26.0f, 0xC0000000);
	const GfxImage *img = art_get(card_poster(&c), ART_POSTER);
	if (img)
		gfx_image_cover(img, px, py, pw, ph, 0xFFFFFFFF);
	else {
		gfx_round_rect(px, py, pw, ph, 12.0f, C_PANEL2);
		gfx_icon(ICON_FILM, px + pw * 0.5f - 30.0f, py + ph * 0.4f, 60.0f, C_FAINT);
	}
	const float x = px + pw + 48.0f;
	float y = py - 6.0f;
	const float w = CONTENT_R - x;
	if (m->kind == MEDIA_EPISODE) {
		char kicker[160];
		snprintf(kicker, sizeof kicker, "%s  \xC2\xB7  SEASON %u  \xC2\xB7  EPISODE %u", m->show, (unsigned)m->season,
			(unsigned)m->episode);
		gfx_text_fit(FONT_SMALL, x, y, w, C_LIME, kicker);
		y += 28.0f;
	}
	gfx_text_fit(FONT_HERO, x - 2.0f, y, w, C_TEXT, m->title);
	y += 84.0f;
	char year[8] = "", runtime[16] = "", size[24] = "";
	if (m->year)
		snprintf(year, sizeof year, "%u", (unsigned)m->year);
	const uint32_t minutes = m->runtime_min ? m->runtime_min : v->probe.duration_s / 60;
	library_format_runtime(minutes, runtime, sizeof runtime);
	format_size(m->size_bytes, size, sizeof size);
	const char *parts[] = { year, runtime, m->genre, m->container };
	draw_meta_line(x, y, parts, 4, C_DIM);
	y += 44.0f;
	if (m->plot[0]) {
		y += gfx_text_wrap(FONT_BODY, x, y, w, 5, C_TEXT, m->plot) + 10.0f;
	} else {
		gfx_text_wrap(FONT_BODY, x, y, w, 2, C_FAINT,
			"No description. A Kodi-style .nfo beside the file (or from the PC companion) fills this in.");
		y += 70.0f;
	}
	// Progress.
	if (u->position_s && !u->watched) {
		char pos[24], dur[24], line[80];
		format_clock(u->position_s, pos, sizeof pos);
		format_clock(u->duration_s, dur, sizeof dur);
		snprintf(line, sizeof line, "Stopped at %s of %s", pos, dur);
		gfx_text(FONT_SMALL, x, y, C_DIM, line);
		const float frac = u->duration_s ? (float)u->position_s / (float)u->duration_s : 0.0f;
		gfx_rect(x, y + 30.0f, 320.0f, 5.0f, C_LINE);
		gfx_rect(x, y + 30.0f, 320.0f * frac, 5.0f, C_LIME);
		y += 50.0f;
	}
	// The buttons.
	DetailAction acts[8];
	const int n = detail_actions(v->item, acts);
	float bx = x;
	const float by = py + ph - 54.0f;
	for (int i = 0; i < n; i++) {
		int icon = ICON_PLAY;
		const char *label = "Play";
		switch (acts[i]) {
		case ACT_RESUME:
			label = "Resume";
			break;
		case ACT_PLAY:
			label = u->position_s && !u->watched ? "Start over" : "Play";
			icon = u->position_s && !u->watched ? ICON_REWIND : ICON_PLAY;
			break;
		case ACT_NEXT:
			label = "Next episode";
			icon = ICON_FORWARD;
			break;
		case ACT_FAVORITE:
			label = u->favorite ? "Favorite" : "Add to Favorites";
			icon = u->favorite ? ICON_HEART_FILL : ICON_HEART;
			break;
		case ACT_WATCHED:
			label = u->watched ? "Watched" : "Mark watched";
			icon = ICON_CHECK;
			break;
		case ACT_SHOW:
			label = "All episodes";
			icon = ICON_LIST;
			break;
		}
		// Past the first two, icons alone keep the row on screen.
		const bool compact = i >= 2 && n > 3;
		const float bw = compact ? 54.0f + 8.0f : button_width(icon, label);
		if (compact) {
			const bool focused = v->sel == i;
			gfx_round_rect(bx, by, 54.0f, 54.0f, 12.0f, focused ? C_LIME : 0x26FFFFFF);
			gfx_icon(icon, bx + 13.0f, by + 13.0f, 28.0f, focused ? C_ON_LIME : C_TEXT);
			if (focused)
				gfx_text(FONT_SMALL, x, by + 68.0f, C_DIM, label);
			bx += 54.0f + 14.0f;
		} else {
			draw_button(bx, by, bw, icon, label, v->sel == i, i == 0);
			bx += bw + 14.0f;
		}
	}
	// The file.
	float fy = py + ph + 50.0f;
	gfx_text(FONT_SMALL, CONTENT_X, fy, C_LIME, "FILE");
	fy += 28.0f;
	char line[200];
	snprintf(line, sizeof line, "%s  \xC2\xB7  %s", m->container, size);
	gfx_text(FONT_SMALL, CONTENT_X, fy, C_DIM, line);
	gfx_text_fit(FONT_SMALL, CONTENT_X + 260.0f, fy, CONTENT_R - CONTENT_X - 260.0f, C_FAINT, m->path);
	fy += 26.0f;
	if (v->probe.ok) {
		char fps[16] = "";
		if (v->probe.fps_milli)
			snprintf(fps, sizeof fps, "%.3g fps", (double)v->probe.fps_milli / 1000.0);
		snprintf(line, sizeof line, "Video %s %ux%u %s   \xC2\xB7   Audio %s %u Hz %uch%s",
			v->probe.video[0] ? v->probe.video : "none", (unsigned)v->probe.width, (unsigned)v->probe.height, fps,
			v->probe.audio[0] ? v->probe.audio : "none", (unsigned)v->probe.audio_rate,
			(unsigned)v->probe.audio_channels, v->probe.moov_first ? "" : "   \xC2\xB7   index at the end");
		gfx_text_fit(FONT_SMALL, CONTENT_X, fy, CONTENT_R - CONTENT_X, C_DIM, line);
	} else if (v->probe.note[0]) {
		gfx_text(FONT_SMALL, CONTENT_X, fy, C_FAINT, v->probe.note);
	}
	const char *hints[] = { "Y", "Favorite", "B", "Back", "A", "Select" };
	draw_hints(hints, 3);
}

// --------------------------------------------------------------------------------------------------------------------
// a show: seasons and episodes

static int season_count(const MediaShow *s, int *firsts, int max)
{
	int n = 0;
	int last = -1;
	for (uint32_t e = 0; e < s->count; e++) {
		const int season = lib()->items[lib()->episodes[s->first + e]].season;
		if (season != last) {
			if (n < max)
				firsts[n] = (int)e;
			n++;
			last = season;
		}
	}
	return n;
}

static void update_show(View *v)
{
	const MediaShow *s = &lib()->shows[v->show];
	int firsts[64];
	const int seasons = season_count(s, firsts, 64);
	const int capped = seasons > 64 ? 64 : seasons;
	if (v->season >= capped)
		v->season = capped - 1;
	const int start = firsts[v->season];
	const int end = v->season + 1 < capped ? firsts[v->season + 1] : (int)s->count;
	const int n = end - start;
	const uint32_t p = ui.input.pressed;
	if (p & IN_LB && v->season > 0) {
		v->season--;
		v->sel = 0;
	}
	if (p & IN_RB && v->season < capped - 1) {
		v->season++;
		v->sel = 0;
	}
	if (p & IN_UP && v->sel > 0)
		v->sel--;
	if (p & IN_DOWN && v->sel < n - 1)
		v->sel++;
	if (n <= 0)
		return;
	const uint32_t item = lib()->episodes[s->first + (uint32_t)(start + v->sel)];
	if (p & IN_A) {
		const MediaUser *u = &lib()->user[item];
		start_playback(item, !(u->position_s && !u->watched));
	}
	if (p & IN_X) {
		View d;
		memset(&d, 0, sizeof d);
		d.kind = VIEW_DETAILS;
		d.item = item;
		push(d);
	}
	if (p & IN_Y)
		toggle_watched(item);
	if (p & IN_START) {
		// The show's favourite mark lives on its first episode.
		toggle_favorite(lib()->episodes[s->first]);
	}
}

static void draw_show(View *v)
{
	const MediaShow *s = &lib()->shows[v->show];
	const Card c = { .kind = CARD_SHOW, .index = v->show };
	draw_backdrop(card_backdrop(&c), 1.0f);
	gfx_text(FONT_SMALL, CONTENT_X, 58.0f, C_LIME, "TV SHOW");
	gfx_text_fit(FONT_HERO, CONTENT_X - 2.0f, 82.0f, 900.0f, C_TEXT, s->name);
	char count[32], year[8] = "";
	snprintf(count, sizeof count, "%u episodes", (unsigned)s->count);
	if (s->year)
		snprintf(year, sizeof year, "%u", (unsigned)s->year);
	const char *parts[] = { year, count, s->genre, card_favorite(&c) ? "Favorite" : "" };
	draw_meta_line(CONTENT_X, 162.0f, parts, 4, C_DIM);
	if (s->plot[0])
		gfx_text_wrap(FONT_BODY, CONTENT_X, 198.0f, 700.0f, 2, C_TEXT, s->plot);

	int firsts[64];
	const int seasons = season_count(s, firsts, 64);
	const int capped = seasons > 64 ? 64 : seasons;
	// Season tabs.
	float tx = CONTENT_X;
	const float ty = 270.0f;
	for (int i = 0; i < capped; i++) {
		const uint8_t number = lib()->items[lib()->episodes[s->first + (uint32_t)firsts[i]]].season;
		char label[24];
		if (number)
			snprintf(label, sizeof label, "Season %u", (unsigned)number);
		else
			snprintf(label, sizeof label, "Specials");
		const float w = gfx_text_width(FONT_LABEL, label) + 36.0f;
		if (tx + w > CONTENT_R)
			break;
		const bool current = i == v->season;
		gfx_round_rect(tx, ty, w, 46.0f, 23.0f, current ? C_LIME : 0x26FFFFFF);
		gfx_text(FONT_LABEL, tx + 18.0f, ty + 10.0f, current ? C_ON_LIME : C_DIM, label);
		tx += w + 12.0f;
	}
	if (capped > 1)
		gfx_text(FONT_SMALL, tx + 8.0f, ty + 13.0f, C_FAINT, "LB / RB");
	// Episodes.
	const int start = capped ? firsts[v->season] : 0;
	const int end = v->season + 1 < capped ? firsts[v->season + 1] : (int)s->count;
	const float row_h = 92.0f;
	const float list_y = 340.0f;
	float target = (float)v->sel * row_h - row_h * 1.5f;
	if (target < 0.0f)
		target = 0.0f;
	v->scroll = approach(v->scroll, target, 0.22f);
	gfx_clip(CONTENT_X - 10.0f, list_y - 6.0f, CONTENT_R - CONTENT_X + 20.0f, SCREEN_H - 70.0f - list_y);
	for (int e = start; e < end; e++) {
		const uint32_t item = lib()->episodes[s->first + (uint32_t)e];
		const MediaItem *m = &lib()->items[item];
		const MediaUser *u = &lib()->user[item];
		const float y = list_y + (float)(e - start) * row_h - v->scroll;
		if (y > SCREEN_H || y + row_h < list_y - row_h)
			continue;
		const bool focused = e - start == v->sel;
		gfx_round_rect(CONTENT_X, y, 900.0f, row_h - 12.0f, 12.0f, focused ? C_PANEL2 : 0x80181E24);
		if (focused)
			gfx_round_outline(CONTENT_X - 2.0f, y - 2.0f, 904.0f, row_h - 8.0f, 13.0f, C_LIME);
		char number[8];
		snprintf(number, sizeof number, "%u", (unsigned)m->episode);
		gfx_text(FONT_TITLE, CONTENT_X + 24.0f, y + 18.0f, focused ? C_LIME : C_FAINT, number);
		gfx_text_fit(FONT_LABEL, CONTENT_X + 96.0f, y + 12.0f, 640.0f, C_TEXT, m->title);
		char sub[80] = "";
		char runtime[16];
		library_format_runtime(m->runtime_min, runtime, sizeof runtime);
		if (u->position_s && !u->watched) {
			char pos[16];
			format_clock(u->position_s, pos, sizeof pos);
			snprintf(sub, sizeof sub, "Stopped at %s", pos);
		} else {
			snprintf(sub, sizeof sub, "%s%s%s", runtime, runtime[0] ? "  \xC2\xB7  " : "", m->container);
		}
		gfx_text(FONT_SMALL, CONTENT_X + 96.0f, y + 44.0f, C_DIM, sub);
		if (u->watched) {
			gfx_circle(CONTENT_X + 860.0f, y + 40.0f, 14.0f, C_LIME);
			gfx_icon(ICON_CHECK, CONTENT_X + 848.0f, y + 28.0f, 24.0f, C_ON_LIME);
		} else if (u->position_s && u->duration_s) {
			const float frac = (float)u->position_s / (float)u->duration_s;
			gfx_rect(CONTENT_X + 96.0f, y + row_h - 22.0f, 300.0f, 4.0f, C_LINE);
			gfx_rect(CONTENT_X + 96.0f, y + row_h - 22.0f, 300.0f * frac, 4.0f, C_LIME);
		}
		if (focused)
			gfx_icon(ICON_PLAY, CONTENT_X + 800.0f, y + 26.0f, 28.0f, C_LIME);
	}
	gfx_unclip();
	gfx_text(FONT_SMALL, CONTENT_X, (float)SCREEN_H - 48.0f, C_FAINT, "START  Favorite show");
	const char *hints[] = { "X", "Details", "Y", "Watched", "A", "Play" };
	draw_hints(hints, 3);
}

// --------------------------------------------------------------------------------------------------------------------
// the player

enum {
	OVERLAY_FRAMES = 240,
	SAVE_EVERY_FRAMES = 600,
	NEXT_EPISODE_FRAMES = 600,
};

// Saves where the viewer is; past 92% of the way through counts as watched.
static void save_progress(uint32_t item, bool ended)
{
	MediaUser *u = library_user(item);
	if (!u)
		return;
	PlayerStatus st;
	player_status(&st);
	if (st.duration > 0.0)
		u->duration_s = (uint32_t)st.duration;
	const bool finished = ended || (st.duration > 0.0 && st.position >= st.duration * 0.92);
	if (finished) {
		u->watched = true;
		u->position_s = 0;
	} else if (st.position >= 10.0) {
		u->position_s = (uint32_t)st.position;
	}
	u->last_played = now_seconds();
	library_user_changed();
}

static void leave_player(View *v)
{
	PlayerStatus st;
	player_status(&st);
	if (st.state != PLAYER_FAILED && st.state != PLAYER_OPENING)
		save_progress(v->item, st.state == PLAYER_ENDED);
	player_close();
	data_changed();
	pop();
}

static void play_next_episode(View *v)
{
	const uint32_t next = library_next_episode(v->item);
	if (next == UINT32_MAX)
		return;
	save_progress(v->item, true);
	player_close();
	v->item = next;
	v->probed = false;
	ui.overlay_frames = 0;
	ui.ended_frame = 0;
	MediaUser *u = library_user(next);
	const double start = u && u->position_s > 5 && !u->watched ? (double)u->position_s - 3.0 : 0.0;
	player_open(lib()->items[next].path, start, ui.settings.volume);
	data_changed();
}

static void update_player(View *v)
{
	const uint32_t p = ui.input.pressed;
	PlayerStatus st;
	player_status(&st);
	if (p)
		ui.overlay_frames = 0;
	else
		ui.overlay_frames++;
	if (p & IN_B) {
		leave_player(v);
		return;
	}
	if (st.state == PLAYER_FAILED)
		return;
	if (p & IN_Y)
		ui.player_stats = !ui.player_stats;
	if (st.state == PLAYER_ENDED) {
		const bool has_next = library_next_episode(v->item) != UINT32_MAX;
		if (!ui.ended_frame)
			ui.ended_frame = ui.frame;
		if (!has_next) {
			leave_player(v);
			return;
		}
		if (p & IN_A || (ui.settings.autoplay_next && ui.frame - ui.ended_frame >= NEXT_EPISODE_FRAMES))
			play_next_episode(v);
		return;
	}
	if (p & (IN_A | IN_START))
		player_set_paused(st.state != PLAYER_PAUSED);
	double step = 0.0;
	if (p & IN_LEFT)
		step = -10.0;
	if (p & IN_RIGHT)
		step = 10.0;
	if (p & IN_LT)
		step = -60.0;
	if (p & IN_RT)
		step = 60.0;
	if (step != 0.0 && st.duration > 0.0)
		player_seek(st.position + step);
	if (p & IN_RB && library_next_episode(v->item) != UINT32_MAX)
		play_next_episode(v);
	if (ui.frame - ui.progress_saved_frame >= SAVE_EVERY_FRAMES && st.state == PLAYER_PLAYING) {
		ui.progress_saved_frame = ui.frame;
		save_progress(v->item, false);
	}
}

static void draw_spinner(float cx, float cy)
{
	for (int i = 0; i < 8; i++) {
		const float a = (float)i / 8.0f * 6.2831853f;
		const float phase = fmodf(ui.time * 1.2f - (float)i / 8.0f + 1.0f, 1.0f);
		gfx_circle(cx + cosf(a) * 22.0f, cy + sinf(a) * 22.0f, 5.0f, with_alpha(C_LIME, 0.25f + 0.75f * phase));
	}
}

static void draw_player(View *v)
{
	const MediaItem *m = &lib()->items[v->item];
	player_update();
	PlayerStatus st;
	player_status(&st);
	gfx_rect(0.0f, 0.0f, SCREEN_W, SCREEN_H, 0xFF000000);
	const bool picture = player_draw(0.0f, 0.0f, SCREEN_W, SCREEN_H);
	const float cx = SCREEN_W * 0.5f;
	if (st.state == PLAYER_FAILED) {
		gfx_round_rect(cx - 360.0f, 260.0f, 720.0f, 200.0f, 18.0f, C_PANEL);
		const char *head = "This video can't play";
		gfx_text(FONT_TITLE, cx - gfx_text_width(FONT_TITLE, head) * 0.5f, 290.0f, C_TEXT, head);
		gfx_text_wrap(FONT_BODY, cx - 320.0f, 350.0f, 640.0f, 2, C_DIM, st.error);
		const char *hints[] = { "B", "Back" };
		draw_hints(hints, 1);
		return;
	}
	if (st.state == PLAYER_OPENING || (st.state == PLAYER_BUFFERING && !picture)) {
		draw_spinner(cx, SCREEN_H * 0.5f);
		const char *msg = "Loading";
		gfx_text(FONT_BODY, cx - gfx_text_width(FONT_BODY, msg) * 0.5f, SCREEN_H * 0.5f + 44.0f, C_DIM, msg);
	} else if (st.state == PLAYER_BUFFERING) {
		draw_spinner(cx, SCREEN_H * 0.5f);
	}
	const bool show = st.state == PLAYER_PAUSED || ui.overlay_frames < OVERLAY_FRAMES || st.state != PLAYER_PLAYING;
	if (st.state == PLAYER_ENDED) {
		const uint32_t next = library_next_episode(v->item);
		if (next != UINT32_MAX) {
			const MediaItem *n = &lib()->items[next];
			const float w = 560.0f, h = 170.0f, x = SCREEN_W - w - 60.0f, y = SCREEN_H - h - 90.0f;
			gfx_shadow(x, y, w, h, 26.0f, 0xC0000000);
			gfx_round_rect(x, y, w, h, 16.0f, C_PANEL);
			gfx_text(FONT_SMALL, x + 26.0f, y + 22.0f, C_LIME, "UP NEXT");
			char line[160];
			snprintf(line, sizeof line, "S%u E%u  \xC2\xB7  %s", (unsigned)n->season, (unsigned)n->episode, n->title);
			gfx_text_fit(FONT_LABEL, x + 26.0f, y + 50.0f, w - 52.0f, C_TEXT, line);
			if (ui.settings.autoplay_next) {
				const uint32_t left = ui.frame - ui.ended_frame >= NEXT_EPISODE_FRAMES
										  ? 0
										  : (NEXT_EPISODE_FRAMES - (ui.frame - ui.ended_frame) + 59) / 60;
				snprintf(line, sizeof line, "Starting in %u", (unsigned)left);
				gfx_text(FONT_SMALL, x + 26.0f, y + 84.0f, C_DIM, line);
			}
			draw_button(x + 26.0f, y + 104.0f, button_width(ICON_PLAY, "Play now"), ICON_PLAY, "Play now", true, true);
		}
	}
	if (show) {
		gfx_gradient_v(0.0f, 0.0f, SCREEN_W, 170.0f, 0xD0000000, 0x00000000);
		gfx_gradient_v(0.0f, SCREEN_H - 200.0f, SCREEN_W, 200.0f, 0x00000000, 0xE0000000);
		if (m->kind == MEDIA_EPISODE) {
			char line[160];
			snprintf(line, sizeof line, "%s  \xC2\xB7  S%u E%u", m->show, (unsigned)m->season, (unsigned)m->episode);
			gfx_text(FONT_SMALL, 70.0f, 44.0f, C_LIME, line);
		}
		gfx_text_fit(FONT_TITLE, 70.0f, m->kind == MEDIA_EPISODE ? 68.0f : 50.0f, 1000.0f, C_TEXT, m->title);
		// The bar and the times.
		const float bx = 70.0f, by = SCREEN_H - 92.0f, bw = SCREEN_W - 140.0f;
		const float frac = st.duration > 0.0 ? (float)(st.position / st.duration) : 0.0f;
		gfx_round_rect(bx, by, bw, 6.0f, 3.0f, 0x50FFFFFF);
		gfx_round_rect(bx, by, bw * (frac > 1.0f ? 1.0f : frac), 6.0f, 3.0f, C_LIME);
		gfx_circle(bx + bw * (frac > 1.0f ? 1.0f : frac), by + 3.0f, 9.0f, C_TEXT);
		char pos[24], dur[24], times[64];
		format_clock((uint32_t)st.position, pos, sizeof pos);
		format_clock((uint32_t)st.duration, dur, sizeof dur);
		snprintf(times, sizeof times, "%s / %s", pos, dur);
		gfx_icon(st.state == PLAYER_PAUSED ? ICON_PLAY : ICON_PAUSE, bx, by + 24.0f, 30.0f, C_TEXT);
		gfx_text(FONT_LABEL, bx + 46.0f, by + 26.0f, C_TEXT, times);
		const char *help = "A Pause   \xE2\x86\x90 \xE2\x86\x92 10 s   LT RT 1 min   Y Stats   B Back";
		gfx_text(FONT_SMALL, bx + bw - gfx_text_width(FONT_SMALL, help), by + 30.0f, C_DIM, help);
	}
	if (ui.player_stats) {
		const float x = SCREEN_W - 450.0f, y = 40.0f;
		gfx_round_rect(x, y, 400.0f, 276.0f, 12.0f, 0xD0101418);
		char line[128];
		float ly = y + 16.0f;
#define STAT_LINE(...)                                                                                                 \
	do {                                                                                                               \
		snprintf(line, sizeof line, __VA_ARGS__);                                                                      \
		gfx_text(FONT_SMALL, x + 18.0f, ly, C_TEXT, line);                                                             \
		ly += 26.0f;                                                                                                   \
	} while (0)
		STAT_LINE("%s %dx%d  %.2f fps", st.video_codec, st.width, st.height, st.fps);
		STAT_LINE("Audio %s", st.audio_codec[0] ? st.audio_codec : "none");
		STAT_LINE("Decoding %.1f fps on %d threads", (double)st.decode_fps, st.threads);
		STAT_LINE("Quality: %s", st.level_name);
		STAT_LINE("Shown %u  dropped %u", (unsigned)st.shown, (unsigned)st.dropped);
		STAT_LINE("Queued %u frames, %.2f s audio", (unsigned)st.queued_frames, (double)st.audio_buffered_s);
		STAT_LINE("Audio underruns %u", (unsigned)st.audio_underruns);
		GfxStats gs;
		gfx_stats(&gs);
		STAT_LINE("Frame %.1f ms avg, %u missed vblanks", (double)gs.frame_ms_avg, (unsigned)gs.missed_vblanks);
		STAT_LINE("At %.1f s of %.1f s", st.position, st.duration);
#undef STAT_LINE
	}
}

// --------------------------------------------------------------------------------------------------------------------
// diagnostics

static void drive_line(const char *drive, const char *label, char *out, size_t size)
{
	struct statvfs vfs;
	if (statvfs(drive, &vfs) == 0) {
		const double block = (double)vfs.f_frsize ? (double)vfs.f_frsize : (double)vfs.f_bsize;
		const double total = (double)vfs.f_blocks * block / (1024.0 * 1024.0 * 1024.0);
		const double free_gb = (double)vfs.f_bavail * block / (1024.0 * 1024.0 * 1024.0);
		snprintf(out, size, "%-6s %s: %.1f GB free of %.1f GB", drive, label, free_gb, total);
	} else {
		snprintf(out, size, "%-6s %s: not connected", drive, label);
	}
}

// The decoding test: each run decodes one file as fast as it can for 8 s, with a number of threads and a quality
// level, so one test on the console shows what each setting is worth.
typedef struct BenchRun {
	const char *match;
	int threads;
	int level;
	bool vector;
	const char *label;
} BenchRun;

static const BenchRun BENCH_PLAN[] = {
	{ "1080p", 5, 2, false, "1080p, 5 threads, no deblock" },
	{ "1080p", 6, 2, false, "1080p, 6 threads, no deblock" },
	{ "1080p", 6, 0, false, "1080p, 6 threads" },
	{ "1080p", 6, 1, false, "1080p, 6 threads, no B deblock" },
	{ "720p", 6, 0, false, "720p, 6 threads" },
	{ "480p", 6, 0, false, "480p, 6 threads" },
};
// Y: the same 1080p decoded without and with the vector unit, pictures compared, then its speed.
static const BenchRun VECTOR_PLAN[] = {
	{ "1080p", 6, 0, false, "1080p, vector off" },
	{ "1080p", 6, 0, true, "1080p, vector on" },
	{ "1080p", 6, 2, true, "1080p, vector on, no deblock" },
	{ "720p", 6, 0, true, "720p, vector on" },
};
enum {
	BENCH_SECONDS = 8,
};

// The library's "Shmees Test <match>" movie, or else its first movie.
static uint32_t bench_item(const char *match)
{
	const Library *l = lib();
	for (uint32_t i = 0; i < l->movie_count; i++) {
		const char *title = l->items[l->movies[i]].title;
		if (!strncasecmp(title, "Shmees Test", 11) && strcasestr(title, match))
			return l->movies[i];
	}
	return l->movie_count ? l->movies[0] : UINT32_MAX;
}

static void start_benchmark(const BenchRun *plan, uint32_t runs)
{
	if (!lib()->movie_count) {
		toast("No movies to test with");
		return;
	}
	ui.bench_plan = plan;
	ui.bench_count = 0;
	for (uint32_t i = 0; i < runs; i++) {
		const uint32_t item = bench_item(plan[i].match);
		if (item != UINT32_MAX)
			ui.bench_items[ui.bench_count++] = item;
	}
	memset(ui.bench_results, 0, sizeof ui.bench_results);
	ui.bench_next = 0;
	ui.bench_running = true;
	smc_log("benchmark: starting %u runs", (unsigned)ui.bench_count);
}

static void update_diagnostics(void)
{
	if (ui.input.pressed & IN_X && !ui.bench_running)
		start_benchmark(BENCH_PLAN, sizeof BENCH_PLAN / sizeof BENCH_PLAN[0]);
	if (ui.input.pressed & IN_Y && !ui.bench_running) {
		if (ui.vector_probe == PROBE_CLEAR)
			start_benchmark(VECTOR_PLAN, sizeof VECTOR_PLAN / sizeof VECTOR_PLAN[0]);
		else
			toast("Run the VMX probe first: it has to clear the vector unit");
	}
	if (!ui.bench_running)
		return;
	PlayerBenchmark r;
	player_benchmark_result(&r);
	if (r.running)
		return;
	if (ui.bench_next > 0 && r.done)
		ui.bench_results[ui.bench_next - 1] = r;
	if (ui.bench_next < ui.bench_count) {
		const BenchRun *run = &ui.bench_plan[ui.bench_next];
		player_benchmark_start(lib()->items[ui.bench_items[ui.bench_next]].path, BENCH_SECONDS, run->threads,
			run->level, run->vector, run->label);
		ui.bench_next++;
	} else {
		ui.bench_running = false;
		if (ui.bench_plan == VECTOR_PLAN && ui.bench_count >= 2) {
			// The same frames decoded both ways must be the same pictures, bit for bit.
			const PlayerBenchmark *a = &ui.bench_results[0];
			const PlayerBenchmark *b = &ui.bench_results[1];
			const uint32_t n = a->checksum_count < b->checksum_count ? a->checksum_count : b->checksum_count;
			uint32_t first_bad = UINT32_MAX;
			for (uint32_t i = 0; i < n && first_bad == UINT32_MAX; i++)
				if (a->checksums[i] != b->checksums[i])
					first_bad = i;
			ui.vector_compared = n;
			ui.vector_first_bad = first_bad;
			smc_log("benchmark: vector unit pictures: %s (%u compared%s)", first_bad == UINT32_MAX && n ? "identical" : "DIFFERENT",
				(unsigned)n, first_bad == UINT32_MAX ? "" : ", first difference at a frame");
		}
	}
}

static void draw_diagnostics(void)
{
	gfx_text(FONT_TITLE, CONTENT_X, TOP_Y, C_TEXT, "Diagnostics");
	float y = TOP_Y + 70.0f;
	const float col2 = 680.0f;
	char line[200];
	// Drives (statvfs is a quick kernel query; this screen is open rarely).
	gfx_text(FONT_SMALL, CONTENT_X, y, C_LIME, "STORAGE");
	float sy = y + 30.0f;
	static const char *const drives[][2] = { { "hdd:", "Hard drive (Hdd1)" }, { "usb0:", "USB 0" },
		{ "usb1:", "USB 1" }, { "usb2:", "USB 2" }, { "game:", "This app's folder" } };
	enum { DRIVES = sizeof drives / sizeof drives[0] };
	// Asked of the kernel every two seconds, not every frame.
	static char drive_lines[DRIVES][120];
	static uint32_t drives_at = UINT32_MAX;
	static uint32_t drives_full_at = UINT32_MAX;
	// Every 10 s for drives that answered; a missing drive is asked again every 60 s (each miss puts a line in the
	// log).
	if (drives_at == UINT32_MAX || ui.frame - drives_at >= 600) {
		const bool all = drives_full_at == UINT32_MAX || ui.frame - drives_full_at >= 3600;
		for (size_t i = 0; i < DRIVES; i++)
			if (all || !strstr(drive_lines[i], "not connected"))
				drive_line(drives[i][0], drives[i][1], drive_lines[i], sizeof drive_lines[i]);
		drives_at = ui.frame;
		if (all)
			drives_full_at = ui.frame;
	}
	for (size_t i = 0; i < DRIVES; i++) {
		gfx_text_fit(FONT_SMALL, CONTENT_X, sy, col2 - CONTENT_X - 30.0f, C_DIM, drive_lines[i]);
		sy += 28.0f;
	}
	// Library.
	sy += 16.0f;
	gfx_text(FONT_SMALL, CONTENT_X, sy, C_LIME, "LIBRARY");
	sy += 30.0f;
	ScanStatus st;
	library_scan_status(&st);
	snprintf(line, sizeof line, "%u movies, %u shows, %u episodes", (unsigned)lib()->movie_count,
		(unsigned)lib()->show_count, (unsigned)lib()->episode_count);
	gfx_text(FONT_SMALL, CONTENT_X, sy, C_DIM, line);
	sy += 28.0f;
	snprintf(line, sizeof line, "Last scan: %u folders, %u files, %u .nfo, %u posters, %.1f s", (unsigned)st.folders,
		(unsigned)st.files, (unsigned)st.nfos, (unsigned)st.posters, (double)st.ms / 1000.0);
	gfx_text_fit(FONT_SMALL, CONTENT_X, sy, col2 - CONTENT_X - 30.0f, C_DIM, st.scanning ? "Scanning now" : line);
	sy += 28.0f;
	ArtStats as;
	art_stats(&as);
	snprintf(line, sizeof line, "Artwork: %u held (%.1f MB), %u queued, %u loaded, %u failed", (unsigned)as.held,
		(double)as.held_bytes / (1024.0 * 1024.0), (unsigned)as.queued, (unsigned)as.loaded, (unsigned)as.failed);
	gfx_text_fit(FONT_SMALL, CONTENT_X, sy, col2 - CONTENT_X - 30.0f, C_DIM, line);
	// Video and memory.
	float ry = y;
	gfx_text(FONT_SMALL, col2, ry, C_LIME, "VIDEO");
	ry += 30.0f;
	GfxStats gs;
	gfx_stats(&gs);
	snprintf(line, sizeof line, "Output %ux%u, frame %.2f ms avg, %.1f ms worst", (unsigned)ui.width,
		(unsigned)ui.height, (double)gs.frame_ms_avg, (double)gs.frame_ms_max);
	gfx_text(FONT_SMALL, col2, ry, C_DIM, line);
	ry += 28.0f;
	snprintf(line, sizeof line, "%u quads, %u draws last frame, %u missed vblanks", (unsigned)gs.quads_last_frame,
		(unsigned)gs.draws_last_frame, (unsigned)gs.missed_vblanks);
	gfx_text(FONT_SMALL, col2, ry, C_DIM, line);
	ry += 28.0f;
	snprintf(line, sizeof line, "Textures: %u, %.1f MB", (unsigned)gs.textures_live,
		(double)gs.texture_bytes_live / (1024.0 * 1024.0));
	gfx_text(FONT_SMALL, col2, ry, C_DIM, line);
	ry += 28.0f;
	snprintf(line, sizeof line, "FFmpeg 9.0.2: %d threads, vector %s", player_decode_threads(),
		player_vector() ? "on" : "off");
	gfx_text_fit(FONT_SMALL, col2, ry, CONTENT_R - col2, C_DIM, line);
	ry += 28.0f;
	gfx_text_fit(FONT_SMALL, col2, ry, CONTENT_R - col2, ui.vector_probe == PROBE_MISSING ? C_RED : C_DIM,
		ui.vector_probe_text);
	ry += 44.0f;
	gfx_text(FONT_SMALL, col2, ry, C_LIME, "MEMORY");
	ry += 30.0f;
	MM_QUERY_STATISTICS_RESULT mm;
	memset(&mm, 0, sizeof mm);
	mm.size = sizeof mm;
	if (NT_SUCCESS(MmQueryStatistics(&mm))) {
		snprintf(line, sizeof line, "Free %.0f MB of %.0f MB", (double)mm.title.available_pages * 4.0 / 1024.0,
			(double)mm.total_physical_pages * 4.0 / 1024.0);
		gfx_text(FONT_SMALL, col2, ry, C_DIM, line);
		ry += 28.0f;
		snprintf(line, sizeof line, "This app: %.0f MB heap, %.0f MB image", (double)mm.title.heap_pages * 4.0 / 1024.0,
			(double)mm.title.image_pages * 4.0 / 1024.0);
		gfx_text(FONT_SMALL, col2, ry, C_DIM, line);
	} else {
		gfx_text(FONT_SMALL, col2, ry, C_DIM, "Memory statistics unavailable");
	}
	ry += 44.0f;
	gfx_text(FONT_SMALL, col2, ry, C_LIME, "ABOUT");
	ry += 30.0f;
	snprintf(line, sizeof line, "Shmees Media Center %s, built %s", SMC_VERSION, __DATE__);
	gfx_text(FONT_SMALL, col2, ry, C_DIM, line);
	ry += 28.0f;
	const char *log_path = xsdk_log_path();
	snprintf(line, sizeof line, "Log: %s", log_path ? log_path : "none");
	gfx_text_fit(FONT_SMALL, col2, ry, CONTENT_R - col2, C_DIM, line);
	ry += 28.0f;
	gfx_text(FONT_SMALL, col2, ry, C_FAINT, "Built with xsdk (MIT), FFmpeg (LGPL 2.1), Inter (OFL), stb_image");
	// The decoding test, across the page under both columns.
	float by = 560.0f;
	gfx_text(FONT_SMALL, CONTENT_X, by, C_LIME, "VIDEO DECODING TEST");
	by += 28.0f;
	if (!ui.bench_count) {
		gfx_text(FONT_SMALL, CONTENT_X, by, C_DIM,
			"Press X to time how fast this console decodes the test videos: 6 runs, 8 s each.");
	}
	for (uint32_t i = 0; i < ui.bench_count; i++) {
		const PlayerBenchmark *r = &ui.bench_results[i];
		const char *label = ui.bench_plan[i].label;
		if (i + 1 == ui.bench_next && ui.bench_running) {
			PlayerBenchmark live;
			player_benchmark_result(&live);
			snprintf(line, sizeof line, "%s: testing\xE2\x80\xA6 %.0f s", label, live.seconds);
		} else if (r->done && r->error[0]) {
			snprintf(line, sizeof line, "%s: %s", label, r->error);
		} else if (r->done) {
			snprintf(line, sizeof line, "%s: %.1f fps (%.2fx)", label, r->fps, r->file_fps > 0.0 ? r->fps / r->file_fps : 0.0);
		} else {
			snprintf(line, sizeof line, "%s: waiting", label);
		}
		const float lx = i < 3 ? CONTENT_X : col2;
		const float ly = by + (float)(i % 3) * 26.0f;
		const bool slow = r->done && r->file_fps > 0.0 && r->fps < r->file_fps;
		gfx_text_fit(FONT_SMALL, lx, ly, col2 - CONTENT_X - 30.0f, slow ? C_RED : r->done ? C_LIME : C_DIM, line);
	}
	if (ui.bench_plan == VECTOR_PLAN && !ui.bench_running && ui.vector_compared) {
		const bool same = ui.vector_first_bad == UINT32_MAX;
		if (same)
			snprintf(line, sizeof line, "Pictures with the vector unit: identical (%u frames compared)",
				(unsigned)ui.vector_compared);
		else
			snprintf(line, sizeof line, "Pictures with the vector unit: DIFFERENT from frame %u: keep it off",
				(unsigned)ui.vector_first_bad + 1);
		gfx_text(FONT_SMALL, CONTENT_X, by + 3.0f * 26.0f, same ? C_LIME : C_RED, line);
	}
	const char *hints[] = { "Y", "Test vector unit", "X", "Test decoding", "B", "Back" };
	draw_hints(hints, 3);
}

// --------------------------------------------------------------------------------------------------------------------
// splash, dialog, toast

static float ease_out(float t)
{
	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

static void draw_splash(void)
{
	const float t = (float)ui.splash_frame / 60.0f;
	gfx_rect(0.0f, 0.0f, SCREEN_W, SCREEN_H, C_BG);
	const float in = ease_out(t / 0.6f);
	const float out = t > 1.5f ? 1.0f - ease_out((t - 1.5f) / 0.5f) : 1.0f;
	const float size = 120.0f + 20.0f * in;
	const float cx = SCREEN_W * 0.5f;
	const float cy = SCREEN_H * 0.5f - 40.0f;
	// The lime sweep behind the badge.
	const float sweep = ease_out((t - 0.2f) / 0.8f);
	gfx_rect(cx - 300.0f * sweep, cy + size * 0.5f + 40.0f, 600.0f * sweep, 3.0f, with_alpha(C_LIME, out));
	draw_badge(cx - size * 0.5f, cy - size * 0.5f, size, in * out);
	const float word = ease_out((t - 0.35f) / 0.5f);
	const char *name = "SHMEES";
	gfx_text(FONT_HERO, cx - gfx_text_width(FONT_HERO, name) * 0.5f, cy + size * 0.5f + 52.0f + 10.0f * (1.0f - word),
		with_alpha(C_TEXT, word * out), name);
	const char *tag = "MEDIA CENTER";
	gfx_text(FONT_LABEL, cx - gfx_text_width(FONT_LABEL, tag) * 0.5f, cy + size * 0.5f + 130.0f,
		with_alpha(C_LIME, word * out), tag);
}

static void draw_confirm(void)
{
	gfx_rect(0.0f, 0.0f, SCREEN_W, SCREEN_H, 0xB0000000);
	const float w = 520.0f;
	const float h = 230.0f;
	const float x = (SCREEN_W - w) * 0.5f;
	const float y = (SCREEN_H - h) * 0.5f;
	gfx_shadow(x, y, w, h, 30.0f, 0xC0000000);
	gfx_round_rect(x, y, w, h, 18.0f, C_PANEL);
	draw_badge(x + 30.0f, y + 30.0f, 48.0f, 1.0f);
	gfx_text(FONT_TITLE, x + 96.0f, y + 34.0f, C_TEXT, "Exit to Aurora?");
	gfx_text(FONT_BODY, x + 32.0f, y + 100.0f, C_DIM, "Your progress and favorites are saved.");
	draw_button(x + 32.0f, y + 150.0f, 210.0f, ICON_BACK, "Exit", ui.confirm_sel == 0, true);
	draw_button(x + 262.0f, y + 150.0f, 210.0f, -1, "Stay", ui.confirm_sel == 1, false);
}

static void draw_toast(void)
{
	if (ui.frame >= ui.toast_until || !ui.toast[0])
		return;
	const uint32_t left = ui.toast_until - ui.frame;
	const float a = left < 20 ? (float)left / 20.0f : 1.0f;
	const float w = gfx_text_width(FONT_BODY, ui.toast) + 70.0f;
	const float x = SCREEN_W - w - 60.0f;
	const float y = 40.0f;
	gfx_shadow(x, y, w, 52.0f, 16.0f, with_alpha(0xA0000000, a));
	gfx_round_rect(x, y, w, 52.0f, 14.0f, with_alpha(C_PANEL2, a));
	gfx_circle(x + 26.0f, y + 26.0f, 7.0f, with_alpha(C_LIME, a));
	gfx_text(FONT_BODY, x + 46.0f, y + 12.0f, with_alpha(C_TEXT, a), ui.toast);
}

static void draw_scan_indicator(void)
{
	ScanStatus st;
	library_scan_status(&st);
	if (!st.scanning)
		return;
	const float x = CONTENT_R - 200.0f;
	const float y = (float)SCREEN_H - 46.0f;
	const float spin = ui.time * 4.0f;
	for (int i = 0; i < 3; i++)
		gfx_circle(x + (float)i * 14.0f, y + 10.0f, 4.0f,
			with_alpha(C_LIME, 0.35f + 0.65f * (0.5f + 0.5f * sinf(spin - (float)i * 0.9f))));
	char line[48];
	snprintf(line, sizeof line, "Scanning \xC2\xB7 %u", (unsigned)st.videos);
	gfx_text(FONT_SMALL, x + 46.0f, y, C_DIM, line);
}

// --------------------------------------------------------------------------------------------------------------------
// the loop

static void update(void)
{
	const uint32_t p = ui.input.pressed;
	if (ui.splash) {
		ui.splash_frame++;
		if (ui.splash_frame >= SPLASH_FRAMES || p & (IN_A | IN_B | IN_START))
			ui.splash = false;
		return;
	}
	if (ui.confirm_exit) {
		if (p & IN_LEFT)
			ui.confirm_sel = 0;
		if (p & IN_RIGHT)
			ui.confirm_sel = 1;
		if (p & IN_B)
			ui.confirm_exit = false;
		if (p & IN_A) {
			if (ui.confirm_sel == 0)
				ui.quit = true;
			ui.confirm_exit = false;
		}
		return;
	}
	if (p & IN_BACK) {
		ui.confirm_exit = true;
		ui.confirm_sel = 1;
		return;
	}
	View *v = top();
	if (v->kind != VIEW_TAB) {
		if (p & IN_B && v->kind != VIEW_PLAYER) {
			pop();
			return;
		}
		switch (v->kind) {
		case VIEW_DETAILS:
			update_details(v);
			break;
		case VIEW_SHOW:
			update_show(v);
			break;
		case VIEW_PLAYER:
			update_player(v);
			break;
		case VIEW_DIAGNOSTICS:
			update_diagnostics();
			break;
		default:
			break;
		}
		return;
	}
	if (ui.rail_focus) {
		if (p & IN_UP && ui.tab > 0)
			ui.tab--;
		if (p & IN_DOWN && ui.tab < TAB_COUNT - 1)
			ui.tab++;
		if (p & (IN_RIGHT | IN_A))
			ui.rail_focus = false;
		if (p & IN_B && ui.tab == TAB_HOME) {
			ui.confirm_exit = true;
			ui.confirm_sel = 1;
		} else if (p & IN_B)
			ui.tab = TAB_HOME;
		return;
	}
	if (p & IN_B) {
		ui.rail_focus = true;
		return;
	}
	switch (ui.tab) {
	case TAB_HOME:
		update_home();
		break;
	case TAB_SEARCH:
		update_search();
		break;
	case TAB_SETTINGS:
		update_settings();
		break;
	default:
		update_grid(ui.tab);
		break;
	}
}

static void draw(void)
{
	if (ui.splash) {
		draw_splash();
		return;
	}
	gfx_gradient_v(0.0f, 0.0f, SCREEN_W, SCREEN_H, 0xFF141A20, C_BG);
	View *v = top();
	switch (v->kind) {
	case VIEW_DETAILS:
		draw_details(v);
		break;
	case VIEW_SHOW:
		draw_show(v);
		break;
	case VIEW_PLAYER:
		draw_player(v);
		break;
	case VIEW_DIAGNOSTICS:
		draw_diagnostics();
		break;
	case VIEW_TAB:
		switch (ui.tab) {
		case TAB_HOME:
			draw_home();
			break;
		case TAB_SEARCH:
			draw_search();
			break;
		case TAB_SETTINGS:
			draw_settings();
			break;
		default:
			draw_grid(ui.tab);
			break;
		}
		ui.rail_open = approach(ui.rail_open * 100.0f, ui.rail_focus ? 100.0f : 0.0f, 0.25f) / 100.0f;
		draw_rail();
		break;
	}
	if (v->kind != VIEW_PLAYER)
		draw_scan_indicator();
	draw_toast();
	if (ui.confirm_exit)
		draw_confirm();
	if (!ui.input.connected && ui.frame > 60) {
		const char *msg = "Connect a controller";
		gfx_round_rect(SCREEN_W * 0.5f - 170.0f, 20.0f, 340.0f, 48.0f, 12.0f, C_RED);
		gfx_text(FONT_LABEL, SCREEN_W * 0.5f - gfx_text_width(FONT_LABEL, msg) * 0.5f, 31.0f, C_TEXT, msg);
	}
}

int app_run(void)
{
	memset(&ui, 0, sizeof ui);
	smc_log("Shmees Media Center %s starting", SMC_VERSION);
	// Raised, so a decoding thread sharing hardware thread 0 never holds up a frame (threads.h).
	threads_set_my_priority(2);
	if (!gfx_open()) {
		smc_log("app: the display didn't open; leaving");
		gfx_close();
		return 1;
	}
	ui.width = SCREEN_W;
	ui.height = SCREEN_H;
	// Settings and the index live beside the .xex, in game:\data; made on the first start.
	struct stat data_dir;
	if (stat("game:/data", &data_dir) != 0)
		mkdir("game:/data", 0777);
	settings_load(&ui.settings, "game:/data");
	library_init("game:/data");
	ui.vector_probe = read_vector_probe(ui.vector_probe_text, sizeof ui.vector_probe_text);
	smc_log("app: %s", ui.vector_probe_text);
	if (ui.settings.vector && ui.vector_probe != PROBE_CLEAR) {
		ui.settings.vector = false;
		settings_save(&ui.settings);
	}
	player_set_vector(ui.settings.vector);
	library_set_roots(ui.settings.roots, ui.settings.root_count);
	art_open();
	ui.depth = 1;
	ui.stack[0].kind = VIEW_TAB;
	ui.tab = TAB_HOME;
	ui.splash = ui.settings.startup_animation;
	ui.data_version = 1;
	if (ui.settings.scan_on_start || !lib()->count)
		library_scan_start();

	while (!ui.quit) {
		input_poll(&ui.input);
		if (library_pending()) {
			// Views hold item and show numbers: find the same files and shows in the new library.
			static char paths[STACK][LIB_PATH];
			for (int d = 1; d < ui.depth; d++) {
				const View *v = &ui.stack[d];
				snprintf(paths[d], sizeof paths[d], "%s",
					v->kind == VIEW_SHOW ? lib()->shows[v->show].name
					: v->kind == VIEW_DIAGNOSTICS ? ""
												  : lib()->items[v->item].path);
			}
			if (library_poll()) {
				data_changed();
				for (int d = 1; d < ui.depth; d++) {
					View *v = &ui.stack[d];
					bool found = false;
					if (v->kind == VIEW_SHOW) {
						for (uint32_t i = 0; i < lib()->show_count && !found; i++)
							if (!strcmp(lib()->shows[i].name, paths[d])) {
								v->show = i;
								found = true;
							}
					} else if (v->kind == VIEW_DIAGNOSTICS) {
						found = true;
					} else {
						for (uint32_t i = 0; i < lib()->count && !found; i++)
							if (!strcmp(lib()->items[i].path, paths[d])) {
								v->item = i;
								found = true;
							}
					}
					if (!found) {
						ui.depth = d;
						break;
					}
				}
				char line[96];
				snprintf(line, sizeof line, "Library: %u movies, %u shows", (unsigned)lib()->movie_count,
					(unsigned)lib()->show_count);
				if (!ui.splash)
					toast(line);
			}
		}
		update();
		art_update();
		if (!gfx_begin_frame())
			break;
		draw();
		gfx_end_frame();
		ui.frame++;
		ui.time = (float)ui.frame / 60.0f;
	}
	smc_log("app: leaving");
	if (top()->kind == VIEW_PLAYER) {
		PlayerStatus st;
		player_status(&st);
		if (st.state != PLAYER_FAILED && st.state != PLAYER_OPENING)
			save_progress(top()->item, st.state == PLAYER_ENDED);
	}
	player_close();
	library_save_user();
	art_close();
	gfx_close();
	return 0;
}
