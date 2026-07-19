# CLAUDE.md — Vita Music Assistant

PS Vita homebrew client for [Music Assistant](https://music-assistant.io). C++17,
[Borealis](https://github.com/xfangfang/borealis) UI at 960×544, built with vitasdk.
Talks to an MA server over a WebSocket API, streams audio via **Sendspin**, and
supports **remote access** over WebRTC (libdatachannel).

## Building & verifying — READ FIRST

- **You cannot compile locally.** There is no Vita toolchain here. All builds run
  in CI via `.github/workflows/build-vpk.yml` (~11 min/build).
- Push to the working branch → the workflow builds the VPK and force-pushes the
  artifacts to the **`build-output`** branch: `VitaMusicAssistant.vpk`,
  `VitaMusicAssistant.elf.gz` (unstripped, for symbolication), and
  `BUILD_INFO.txt` (`run <n> sha <commit>`).
- Check a build via the GitHub MCP tools (`mcp__github__actions_list`
  `list_workflow_runs` on `build-vpk.yml`). The user installs the VPK and reports
  back with logs (`ux0:data/VitaMA/vita_ma.log`) or crash dumps.
- Do **not** put the model identifier in commits, PR text, or code.

## Crash symbolication (psp2dmp)

1. `gunzip` the dump; run `scratchpad/vita-parse-core/main.py <dump> <elf>` to get
   the crashed thread + PC (e.g. `VitaMusicAssistant@1 + 0xNNNNN`).
2. Get the matching ELF from `build-output` (confirm the sha in `BUILD_INFO.txt`
   matches the build the user ran): `git show origin/build-output:VitaMusicAssistant.elf.gz | gunzip > vma.elf`.
3. The ELF is linked at **vaddr 0x81000000**, so symbolicate the **absolute**
   address: `addr2line -e vma.elf -f -p -C 0x810NNNNN` (host `addr2line` works on
   the ARM ELF). Scan the core for `0x81000040..0x81ae6078` pointers to
   reconstruct a rough backtrace, then `addr2line` those.

## Architecture

- **Two singletons, one is live.** `App` (`include/app.h`, `App::instance()`) and
  `Application` (`include/app/application.hpp`, `Application::getInstance()`) each
  hold an `AppSettings`. **`Application` is the one loaded from disk and edited by
  the UI** — read/write settings through `Application::getInstance().getSettings()`.
  `App` mainly holds the resolved Sendspin `playerId`.
- `src/app/ma_client.*` — MA WebSocket API + a **hand-rolled `Json`** type. All
  library/queue/player calls go through `MAClient::sendCommand`. Large list
  responses use the **raw (DOM-free) path** — see below.
- `src/app/sendspin_client.*` — audio streaming (client/hello as a `player@v1`,
  FLAC/PCM frames). Transport (play/pause/skip/seek) is done via the **MA API**
  (`player_queues/*` / `players/cmd/*`), NOT Sendspin control commands — that
  matches the official MA app.
- `src/player/native_audio_player.*` — decodes FLAC (dr_flac) → `sceAudioOut`,
  bypassing mpv, when the "native audio" setting is on. Owns pause/resume/seek-bar
  for local playback.
- `src/app/webrtc_client.*` — remote access (signaling + DTLS-SRTP tunnel).
- `src/view/recycling_grid.*` + `media_item_cell.*` — virtualized grid: NVG-drawn
  covers (no per-cell `brls::Image`), off-screen row culling, scroll-gated GPU
  uploads, progressive cover loader, incremental (frame-sliced) build.

## Gotchas learned the hard way

- **Borealis `View::invalidate()` walks to the root and runs a full
  `YGNodeCalculateLayout` over the whole app tree**, and `Box::addView()` calls
  it. Building N cells by adding rows to an attached box is O(N²) and froze the UI
  for ~11s on 1000 items. Build big view trees **detached**, or **frame-slice** the
  build (see `RecyclingGrid::buildStep`).
- **`MAClient::onMessage` runs on the main thread** (WebSocket dispatch defaults to
  main via `brls::sync`). Anything heavy there (parsing, big loops) freezes the UI.
  - The library response is 2.6 MB / ~40k values; building a `Json` DOM took ~8s.
    It now uses a **DOM-free raw path**: `sendCommandRaw` / `getLibraryItemsRaw`,
    `onMessage` pulls `message_id` cheaply and hands the raw `"result"` substring to
    a `MAClient::rawExtractField` (memchr-based) single-pass parser
    (`LibraryTab::parseMusicItemsRaw`), on a **worker thread**.
  - Cold-start session restore runs `connect()` **before** the main loop, so it
    flips socket dispatch to the receive thread for the handshake
    (`setDispatchOnMainThread(false)`) or it deadlocks for 15s.
- **Async-callback lifetimes:** MA response callbacks fire on the main thread and
  can arrive **after** a worker's busy-wait times out. Never capture worker-stack
  locals by reference into them (use-after-free → data abort). Capture shared state
  by value via `shared_ptr` (see the `showAlbumContextMenuStatic` / `loadTrackList`
  fetch helpers).
- Remote (aiortc) needs mbedTLS built with **DTLS-SRTP**; the CI rebuilds mbedTLS +
  curl from source for ABI consistency (mirror prebuilts drift and cause data
  aborts in `curl_global_init`).
- New resource files (XML/PNG) must be added to the VPK `FILE` list in
  `CMakeLists.txt` or they 404 at runtime and crash on first use.

## Conventions

- Match surrounding style; keep comments at the density of the file you're editing.
- Log to `brls::Logger`; the user's `vita_ma.log` is the primary debugging signal.
- Add timing logs (`std::chrono::steady_clock`) when chasing perf — measure before
  optimizing; several "obvious" fixes here moved the needle far less than expected.
