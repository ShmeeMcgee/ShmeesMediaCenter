// Shmees Media Center: probing MP4 and MOV files (probe.h), after ISO/IEC 14496-12 (boxes) and 14496-15 (avcC).

#define _GNU_SOURCE
#include "probe.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

enum {
	MAX_DEPTH = 8,
};

typedef struct {
	FILE *file;
	uint64_t file_size;
	ProbeInfo *info;
	// The track being read: its handler and timescale.
	char handler[5];
	uint32_t timescale;
	uint64_t track_duration;
	uint32_t sample_count;
	bool seen_mdat;
} Probe;

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)(p[0] << 8 | p[1]);
}

static uint64_t be64(const uint8_t *p)
{
	return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static bool read_at(Probe *pr, uint64_t offset, void *buf, size_t n)
{
	if (fseeko(pr->file, (off_t)offset, SEEK_SET) != 0)
		return false;
	return fread(buf, 1, n, pr->file) == n;
}

static const char *h264_profile(uint8_t p)
{
	switch (p) {
	case 66:
		return "Baseline";
	case 77:
		return "Main";
	case 88:
		return "Extended";
	case 100:
		return "High";
	case 110:
		return "High 10";
	case 122:
		return "High 4:2:2";
	case 244:
		return "High 4:4:4";
	default:
		return "Other";
	}
}

static void parse_boxes(Probe *pr, uint64_t start, uint64_t end, int depth);

// A visual or audio sample entry in stsd, its type the codec's.
static void sample_entry(Probe *pr, uint64_t start, uint64_t size, const char *type)
{
	ProbeInfo *info = pr->info;
	uint8_t head[36];
	if (!strcmp(pr->handler, "vide")) {
		if (size < 8 + 78 || !read_at(pr, start + 8, head, 36))
			return;
		info->width = be16(head + 24);
		info->height = be16(head + 26);
		if (!strcmp(type, "avc1") || !strcmp(type, "avc3")) {
			snprintf(info->video, sizeof info->video, "H.264");
			// avcC among the entry's children, after the 78 bytes of the entry itself.
			uint64_t p = start + 8 + 78;
			const uint64_t end = start + size;
			while (p + 8 <= end) {
				uint8_t box[12];
				if (!read_at(pr, p, box, 12))
					break;
				const uint32_t bsize = be32(box);
				if (!memcmp(box + 4, "avcC", 4)) {
					// configurationVersion, AVCProfileIndication, profile_compatibility, AVCLevelIndication.
					snprintf(info->video, sizeof info->video, "H.264 %s@%u.%u", h264_profile(box[9]),
						(unsigned)(box[11] / 10), (unsigned)(box[11] % 10));
					break;
				}
				if (bsize < 8)
					break;
				p += bsize;
			}
		} else if (!strcmp(type, "hvc1") || !strcmp(type, "hev1"))
			snprintf(info->video, sizeof info->video, "HEVC (H.265)");
		else if (!strcmp(type, "mp4v"))
			snprintf(info->video, sizeof info->video, "MPEG-4 Visual");
		else if (!strcmp(type, "av01"))
			snprintf(info->video, sizeof info->video, "AV1");
		else if (!strcmp(type, "vp09"))
			snprintf(info->video, sizeof info->video, "VP9");
		else
			snprintf(info->video, sizeof info->video, "%s", type);
	} else if (!strcmp(pr->handler, "soun")) {
		if (size < 8 + 28 || !read_at(pr, start + 8, head, 28))
			return;
		info->audio_channels = be16(head + 16);
		info->audio_rate = be32(head + 24) >> 16;
		if (!strcmp(type, "mp4a"))
			snprintf(info->audio, sizeof info->audio, "AAC");
		else if (!strcmp(type, "ac-3"))
			snprintf(info->audio, sizeof info->audio, "AC-3");
		else if (!strcmp(type, "ec-3"))
			snprintf(info->audio, sizeof info->audio, "E-AC-3");
		else if (!strcmp(type, ".mp3") || !strcmp(type, "mp3 "))
			snprintf(info->audio, sizeof info->audio, "MP3");
		else
			snprintf(info->audio, sizeof info->audio, "%s", type);
	}
}

static void leaf(Probe *pr, uint64_t start, uint64_t size, uint64_t header, const char *type)
{
	uint8_t buf[32];
	ProbeInfo *info = pr->info;
	if (!strcmp(type, "mvhd")) {
		if (!read_at(pr, start + header, buf, 32))
			return;
		uint32_t timescale;
		uint64_t duration;
		if (buf[0] == 1) {
			timescale = be32(buf + 20);
			duration = be64(buf + 24);
		} else {
			timescale = be32(buf + 12);
			duration = be32(buf + 16);
		}
		if (timescale)
			info->duration_s = (uint32_t)(duration / timescale);
	} else if (!strcmp(type, "mdhd")) {
		if (!read_at(pr, start + header, buf, 32))
			return;
		if (buf[0] == 1) {
			pr->timescale = be32(buf + 20);
			pr->track_duration = be64(buf + 24);
		} else {
			pr->timescale = be32(buf + 12);
			pr->track_duration = be32(buf + 16);
		}
	} else if (!strcmp(type, "hdlr")) {
		// version and flags, pre_defined, then the handler type. The first in a track is the media's; a QuickTime
		// file has a second in minf, the data handler ("alis"), which mustn't replace it.
		if (!pr->handler[0] && read_at(pr, start + header, buf, 12)) {
			memcpy(pr->handler, buf + 8, 4);
			pr->handler[4] = 0;
		}
	} else if (!strcmp(type, "stsd")) {
		// version and flags, entry count, then the first entry.
		if (!read_at(pr, start + header, buf, 16))
			return;
		const uint32_t esize = be32(buf + 8);
		char etype[5];
		memcpy(etype, buf + 12, 4);
		etype[4] = 0;
		if (esize >= 8 && esize <= size)
			sample_entry(pr, start + header + 8, esize, etype);
	} else if (!strcmp(type, "stsz")) {
		if (read_at(pr, start + header, buf, 12))
			pr->sample_count = be32(buf + 8);
	} else if (!strcmp(type, "mdat")) {
		pr->seen_mdat = true;
	} else if (!strcmp(type, "moov")) {
		info->moov_first = !pr->seen_mdat;
	}
}

static void parse_boxes(Probe *pr, uint64_t start, uint64_t end, int depth)
{
	uint64_t p = start;
	while (p + 8 <= end) {
		uint8_t h[16];
		if (!read_at(pr, p, h, 8))
			return;
		uint64_t size = be32(h);
		uint64_t header = 8;
		char type[5];
		memcpy(type, h + 4, 4);
		type[4] = 0;
		if (size == 1) {
			if (!read_at(pr, p + 8, h + 8, 8))
				return;
			size = be64(h + 8);
			header = 16;
		} else if (size == 0) {
			size = end - p;
		}
		if (size < header || p + size > end)
			return;
		// Full boxes carry a version and flags before their children: meta.
		if (!strcmp(type, "moov") || !strcmp(type, "trak") || !strcmp(type, "mdia") || !strcmp(type, "minf") ||
			!strcmp(type, "stbl")) {
			if (!strcmp(type, "moov"))
				leaf(pr, p, size, header, type);
			if (!strcmp(type, "trak")) {
				pr->handler[0] = 0;
				pr->timescale = 0;
				pr->sample_count = 0;
			}
			if (depth < MAX_DEPTH)
				parse_boxes(pr, p + header, p + size, depth + 1);
			if (!strcmp(type, "trak") && !strcmp(pr->handler, "vide") && pr->timescale && pr->track_duration &&
				pr->sample_count && !pr->info->fps_milli)
				pr->info->fps_milli =
					(uint32_t)((uint64_t)pr->sample_count * 1000ull * pr->timescale / pr->track_duration);
		} else {
			leaf(pr, p, size, header, type);
		}
		p += size;
	}
}

void probe_file(const char *path, ProbeInfo *info)
{
	memset(info, 0, sizeof *info);
	const char *dot = strrchr(path, '.');
	const char *ext = dot ? dot + 1 : "";
	FILE *f = fopen(path, "rb");
	if (!f) {
		snprintf(info->note, sizeof info->note, "Can't open the file");
		return;
	}
	if (!strcasecmp(ext, "mp4") || !strcasecmp(ext, "m4v") || !strcasecmp(ext, "mov") || !strcasecmp(ext, "3gp")) {
		Probe pr = { .file = f, .info = info };
		if (fseeko(f, 0, SEEK_END) == 0)
			pr.file_size = (uint64_t)ftello(f);
		parse_boxes(&pr, 0, pr.file_size, 0);
		info->ok = info->video[0] || info->audio[0];
		if (!info->ok)
			snprintf(info->note, sizeof info->note, "No tracks found in the MP4 boxes");
	} else {
		snprintf(info->note, sizeof info->note, "%s files aren't probed yet", ext[0] ? ext : "These");
	}
	fclose(f);
}
