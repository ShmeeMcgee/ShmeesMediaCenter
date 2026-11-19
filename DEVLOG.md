# Shmees Media Center — development log

What's built, what's been tested and where, and what's left. "Tested on console" means run on the Xbox 360 Slim
(XeUnshackle Max, Aurora); anything not marked that way hasn't been.

## Status at a glance

| Area | Code | Compiles | Tested on console |
|---|---|---|---|
| Toolchain (xsdk, GCC 15.3, newlib 4.6) | built from source | yes | **yes** (0.1.0) |
| Display, 720p renderer, Shmees theme | gfx.c | yes | **yes** (0.1.0) |
| Controller (all four pads, repeat) | input.c | yes | **yes** (0.1.0) |
| Library scan (Hdd1/USB, async, saved index) | library.c | yes | **yes** (0.1.0) |
| Kodi .nfo metadata, posters, backdrops | library.c, art.c | yes | **yes** (0.1.0) |
| Home, Movies, TV, Recent, Continue, Favorites, Search, Settings | app.c | yes | **yes** (0.1.0) |
| Details page, seasons and episodes, SxxEyy | app.c, library.c | yes | **yes** (0.1.0) |
| Diagnostics screen | app.c | yes | **yes** (0.1.0) |
| Startup animation (switchable) | app.c | yes | **yes** (0.1.0) |
| Exit to Aurora | app.c | yes | **yes** (0.1.0) |
| MP4 probe (codec, size, fps, length) | probe.c | yes | **yes** (0.1.0) |
| FFmpeg 9.0.2 port (H.264, HEVC, MPEG-4/2; AAC, AC-3, MP3; MP4, MKV, AVI, TS) | tools/build_ffmpeg.sh | yes | **yes**, H.264/AAC MP4 (0.2.0) |
| Video playback (YUV in a pixel shader, A/V sync, frame dropping) | player.c, gfx.c | yes | **yes** (0.2.0): 1080p behind, see below |
| Audio playback (stereo and 5.1 through the kernel's driver) | player.c | yes | **yes**, stereo (0.2.0), no underruns |
| Player controls, overlay, stats, seek, resume, watched, next episode | app.c | yes | **yes** (0.2.0, user: "everything is working") |
| Quality ladder and 5 decoding threads | player.c | yes | **yes** (0.3.0) |
| Decode speed test (Diagnostics, X) | player.c | yes | **yes** (0.3.0), results below |
| 6 decoding threads, raised UI and reading threads, 1080p starts without deblocking | player.c, threads.c | yes | not yet (0.4.0, in 0.5.0) |
| VMX probe (separate title: tries each vector instruction FFmpeg uses) | vmxprobe/ | yes | not yet (1.0.0) |
| FFmpeg AltiVec code, off by default, switch gated by the probe | player.c, tools/ffmpeg-xsdk.patch | yes | not yet (0.5.0) |
| Vector test: pictures with the vector unit off vs on, checksummed (Diagnostics, Y) | player.c, app.c | yes | not yet (0.5.0) |
| Seeking (←/→, LT/RT) | player.c | yes | **yes** (user, 0.3.0) |
| PC companion (metadata, transcoding, FTP) | not written | — | — |

## Phase 1: feasibility

**Toolchain.** No Microsoft XDK is on the PC (`Xbox360-DevEdition\AUDIT.md`), and none is used. The app is built with
xsdk (github.com/xenosworks/xsdk, MIT): GCC 15.3 for `powerpc-xsdk-elf`, newlib, its own runtime (`libxsdk.a`), a
GPU layer, a GLSL-to-Xenos shader compiler (xost), and `elf2xex.py`. Everything is open source and legal to use.

**The risk xsdk carries.** xsdk's documentation describes RGH/JTAG consoles. This console runs unsigned code through
BadUpdate + XeUnshackle Max instead. Both end up running the same kind of unsigned title from Aurora, so it should
work, but nobody has confirmed it on this setup: the boot test below settles it.

**Video.** The Xbox 360's video hardware decode isn't reachable from homebrew: there's no documented or reverse-
engineered path to it in xsdk, libxenon or anywhere public that I could find, and the spec rules out inventing one.
So playback means decoding on the CPU (3 cores, 6 hardware threads, 3.2 GHz, in-order PowerPC) and drawing the frames
with the GPU. The plan:

1. Port FFmpeg's libavcodec/libavformat (LGPL 2.1, built without GPL parts) to xsdk: H.264 + AAC first.
2. Decode on the CPU with frame threads across the six hardware threads; convert YUV to RGB in a pixel shader so the
   CPU only decodes.
3. Measure frames per second on the console at 480p, 720p and 1080p before promising any of them.

Expectation, to be confirmed by measurement: SD H.264 should play; 720p may need lower bitrates or Baseline/Main
profile; 1080p H.264 in software is unlikely at full frame rate. The PC companion can transcode to whatever the
console proves it can play.

## Phase 2: the boot test (this build)

`ShmeesMediaCenter.xex`, version 0.1.0-boottest. It checks, on the real console:

- that an xsdk title starts from Aurora on XeUnshackle Max at all;
- the 720p display, the renderer, the fonts and the Shmees look;
- the controller;
- reading `Hdd1:\Movies`, `Hdd1:\TV Shows`, `Usb0:\Movies`, `Usb0:\TV Shows`;
- decoding JPEG/PNG posters on the console;
- writing its own files (`data\settings.txt`, `data\library.tsv`, `data\userdata.tsv`, the log) beside the .xex;
- exiting back to Aurora.

Pressing Play opens a screen that says playback isn't in this build. That's deliberate: nothing pretends to play.

### Decisions

- **Renderer:** one shader (texture × colour) and one 2048×512 8-bit atlas for text, icons and shapes; posters are
  their own textures. Quads go to a ring of GPU memory in four blocks with a fence per block, as xsdk's own text
  renderer does. Text is Inter rasterised at five sizes on the PC (tools/gen_assets.py).
- **Metadata:** Kodi-style `.nfo` files (`Movie.nfo`, `movie.nfo`, `tvshow.nfo`), the format tinyMediaManager,
  Jellyfin and Kodi already write, so existing tools work today and the PC companion can write the same files later.
- **Artwork:** `Movie-poster.jpg`, `Movie.jpg`, `poster.jpg`/`folder.jpg` in a movie's own folder; backdrops from
  `-fanart.jpg`, `fanart.jpg`, `backdrop.jpg`. Decoded and scaled on a worker thread (posters to 320×480), uploaded two
  a frame, at most 72 posters and 4 backdrops held.
- **Storage:** plain text files a person can read and fix: `data/settings.txt` (key=value), `data/library.tsv` (the
  index, so the library shows at once on the next start), `data/userdata.tsv` (progress, watched, favourites, by path).
- **TV detection:** a file with S01E02 or 1x02 in its name is an episode; folders directly under a folder whose name
  has TV, Show or Series in it are shows.

## Phase 3: playback (0.2.0-player)

- **FFmpeg 9.0.2**, LGPL, cross-built with xsdk's GCC (tools/build_ffmpeg.sh): no assembly or AltiVec yet (Xenon's
  VMX128 lacks some AltiVec instructions; turning FFmpeg's on is a later, measured step). newlib's `clock()` needs
  `times()`, which xsdk lacks, so src/compat.c supplies `clock()`.
- **Threads:** a read thread (demux + audio decoding), a video thread feeding FFmpeg's four frame threads, an audio
  thread feeding `xsdk_audio`, and the UI. xsdk starts threads on their creator's hardware thread, so the link wraps
  `pthread_create` (src/threads.c) to put FFmpeg's decoding threads on hardware threads 2-5 (cores 1 and 2); the
  read, video, audio, poster and scan threads go on 1; the UI keeps 0.
- **Picture:** the three planes go to linear 8-bit textures (three sets, fenced) and video.frag converts YUV to RGB
  with BT.601 or BT.709, limited or full range.
- **Sync:** the clock follows the audio the driver has taken and runs on with the wall clock between, so a stall in
  either never stops the other. Late frames are dropped; when video falls 0.3 s behind, the decoder skips
  non-reference frames until it catches up.
- **Host check:** the same code with a host-built FFmpeg through the software GPU stand-in: plays the test videos with
  correct colour, overlay, stats, seek, pause, end-of-file to watched, and the decode test.

## Console findings

- 2026-11-18, 0.1.0-boottest on the Slim (XeUnshackle Max, Aurora), reported by the user: starts from Aurora; every
  screen looked right; controller fine; posters load; exits to Aurora. Diagnostics: 1280x720, 0.55 ms a frame
  (12.7 ms worst), no missed vblanks; 448 MB of 512 free; Hdd1 293 GB, USB0 14.9 GB.
- **The hard drive's file names stop at 42 characters (FATX).** `Shmees Test Show - S01E02 - The Second One.mp4` (46)
  never reached Hdd1, so the show had one episode. Names on Hdd1 must be 42 characters or fewer, extension included;
  the PC companion has to shorten them. USB drives (FAT32/exFAT) don't have the limit.

- 2026-11-18, 0.2.0-player, user's photo and log: **real playback on the console.** The 1080p test video
  (H.264 High@4.1, 6 Mb/s, AAC stereo) played with its sound and correct colour, but decoding reached 13-22 fps
  against the file's 30 with B-frames skipped: the log has 354 decoded, 307 shown, 47 dropped, 0 audio underruns.
  The UI drew in 2.1 ms a frame while playing. The background is dark charcoal in person (a phone photo had it
  lighter).
- Found in the log: game:\data was made only after the first settings write had failed (fixed in 0.3: made
  first); statvfs on missing USB drives wrote a log line every 2 s in Diagnostics (now every 60 s).

## Phase 4: speed (0.3.0-speed)

AltiVec in FFmpeg would help most, but VMX128 isn't fully compatible with VMX: it dropped a number of integer
operations (Wikipedia, "AltiVec", VMX128), and which ones isn't documented anywhere I could find. FFmpeg's PowerPC
code would fault on any it uses. So 0.3 takes the safe steps and measures them:
- a fifth decoding thread, on hardware thread 1 (the UI takes under 1 ms a frame on core 0);
- `AV_CODEC_FLAG2_FAST`;
- a quality ladder when video falls 0.25 s behind, a step at most every 1.5 s, one back after 6 s well ahead:
  no deblocking on B-frames, then no deblocking, then B-frames skipped;
- the decoding test runs six settings (1080p with 4 and 5 threads, with and without deblocking; 720p; 480p) so one
  console test shows what each is worth.

### Console results, 0.3.0-speed (2026-11-18, user's photo)

Decoding only, as fast as it goes for 8 s, H.264 High@4.1 test videos at 30 fps, no AltiVec:

| Run | fps | of real time |
|---|---|---|
| 1080p, 4 threads | 20.3 | 0.68x |
| 1080p, 5 threads | 24.3 | 0.81x |
| 1080p, 5 threads, no deblocking on B-frames | 25.4 | 0.85x |
| 1080p, 5 threads, no deblocking | 28.2 | 0.94x |
| 720p, 5 threads | 58.8 | 1.96x |
| 480p, 5 threads | 133.4 | 4.45x |

So **720p and below play at full quality with room to spare**; 1080p is short even without deblocking. The fifth
thread gave 20%, so 0.4 adds a sixth (on hardware thread 0, with the UI raised above it) and starts 1080p H.264
without deblocking. The resume at 10 s in the 0.2 log was the user seeking back, not a bug.

## Phase 5: the vector unit (0.5.0-vector)

FFmpeg is now built with its AltiVec code (`--enable-altivec --disable-vsx --disable-power8`), but it stays **off**
until the console shows it is safe: `av_force_cpu_flags(0)` at every open, so FFmpeg's init picks the plain C
functions. One patch (tools/ffmpeg-xsdk.patch, LGPL notice updated) stops `ff_blockdsp_init_ppc` probing `dcbzl`
when AltiVec is off, so nothing vector runs at all with the switch off.

Which vector instructions VMX128 lacks is not published, so a separate small title finds out:
**ShmeesVMXProbe** (vmxprobe/, title ID 0x53484D56). `gen_probes.py` disassembles FFmpeg's PowerPC objects and
takes every distinct vector and cache instruction they contain (79), each with a real encoding from FFmpeg; memory
forms are re-pointed at an aligned buffer. The probe runs them one at a time, writing "trying X" to
`game:\vmxprobe.txt` (flushed to disk) before each and "ok" after. If one freezes the console, the next start
finds the dangling "trying", marks it MISSING and carries on. The ones most likely missing (VMX128 dropped
multiply-add and multiply-sum integer forms) run last: vmladduhm, vmhraddshs, vmulesh/vmulosh, vmsum*, vsum*, plus
the dcbz family. The result goes to `vmxprobe-result.txt` beside it.

The media center reads that file. Settings → "Vector unit for video" only switches on when every instruction ran;
Diagnostics → Y decodes 8 s of the 1080p test with the vector unit off, then on (checksumming the first 90 pictures
each time: identical or the first frame that differs), then on without deblocking, then 720p on. Host check of that
flow: identical over 90 frames (host only; the console is the real test).

Untested on the console: all of Phase 5.

## Remaining issues

1. 1080p H.264 at full frame rate: 0.5 measures 6 threads and, once the probe has run, the vector unit. If the
   probe finds instructions missing, the FFmpeg functions using them get switched off one by one (or rewritten).
   The fallback is the PC companion converting 1080p files to 720p, which plays at 2x real time.
2. MKV, AVI and WMV: probing and demuxing. FFmpeg covers these once ported; WMV's codecs (WMV3/VC-1) decode slower.
3. Subtitles (.srt beside the file) — not started.
4. PC companion — not started.
5. 1080p output: the UI draws at 1280×720 and the console scales it; native 1080p UI isn't planned until playback is.
6. On-screen keyboard is A–Z and 0–9 only.

## Log

- 2026-11-18: Toolchain built from source in the workspace (binutils 2.47, GCC 15.3 C/C++, newlib 4.6, xsdk).
  Renderer, input, library, artwork, settings, MP4 probe and all eight screens written; compiled clean with
  -Wall -Wextra -Werror (format-truncation off: display strings are cut short on purpose).
- 2026-11-18: Host checks, not the console: the library scanner on a fake folder tree (titles, years, SxxEyy and 1x02,
  .nfo, posters, shows, next episode, saved index and user data; AddressSanitizer clean); the MP4 probe on real H.264/AAC
  MP4s made with ffmpeg (fast-start and not) and an MPEG-4 MOV; and the whole UI driven by a scripted controller through
  a software stand-in for the GPU calls, rendering every screen to images. Fixed from that: years in titles ("Blade
  Runner 2049 (2017)"), release tags as episode titles, QuickTime's second hdlr, plot full stops, the hero's layout,
  the side menu's overlay, and soft posters stretched as backdrops.
- Boot-test build packaged (0.1.0-boottest).
- 2026-11-18: 0.1.0 ran on the console (see Console findings).
- 2026-11-18: FFmpeg 9.0.2 cross-built; player written (read, video, audio threads; YUV shader; A/V sync; controls;
  progress; next episode; decode test). Checked on the host with real decoding. 0.2.0-player packaged.
- 2026-11-18: 0.2 played video and sound on the console (see Console findings). 0.3.0-speed: five threads, quality
  ladder, six-setting decoding test, data folder and log fixes.
- 2026-11-18: 0.3 measured on the console (table above). 0.4.0-speed: six threads, priorities, 1080p starts without
  deblocking; the decoding test compares 5 and 6 threads.
- 2026-11-18: 0.5.0-vector: FFmpeg rebuilt with AltiVec (off by default, one patch), VMX probe title written and
  built (79 instructions), Settings switch gated by the probe, Diagnostics Y vector test with picture checksums.
  Host-checked with the software GPU stand-in; nothing of 0.5 tested on the console yet.
