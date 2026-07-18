# Vita Music Assistant

A native [Music Assistant](https://music-assistant.io/) client for the **PlayStation Vita**.
Browse your library, control any Music Assistant player, and stream music straight to the
Vita — on your home network or from anywhere in the world over an encrypted WebRTC
connection.

Built with [Borealis](https://github.com/natinusala/borealis) (GXM), it renders natively at
960×544 and drives every screen with the D-pad, sticks, touch, and the Vita's own button
footer.

---

## Features

### Connectivity & sign-in
- **Three sign-in modes** on a single focused login card:
  - **Music Assistant** — server URL + username/password.
  - **Home Assistant** — the same, forwarded through the `homeassistant` auth provider.
  - **Remote** — connect to your server from outside your LAN by its **Remote ID**.
- **Remote access over WebRTC** — peer-to-peer data channels via the official
  `signaling.music-assistant.io` server (libdatachannel: libjuice ICE, mbedTLS DTLS with
  DTLS-SRTP, usrsctp). Everything the local WebSocket API does works remotely, including
  audio streaming and artwork.
- **Login over the remote connection** — first-time remote sign-in needs no prior setup:
  credentials are sent as an `auth/login` command over the data channel, exactly like the
  official mobile app.
- **Long-lived tokens** — a fresh login is upgraded to a durable token so future
  connections (especially remote ones) never expire; the server URL and remote route are
  remembered separately, and the app reconnects the way you last connected.
- **QR code scanner** — tap the scan button beside the Remote ID field to read your
  server's Remote Access QR with the Vita camera (live preview, on-device decoding). Accepts
  both bare IDs and URLs that embed one.

### Library browsing
- **Home** — recently played items at a glance.
- **Library** — browse **Artists, Albums, Tracks, and Playlists** with cover art.
- **Search** — search your whole library.
- Cover art is fetched through Music Assistant's canonical `/imageproxy/<id>` endpoint and
  cached/resized server-side for the Vita's limited memory.

### Playback
- **Play to any player** — a built-in player switcher targets any Music Assistant player
  (speakers, other clients) or the Vita itself; queue and transport commands always follow
  the selected player.
- **Play on the Vita** — audio streams to the device over the Music Assistant *sendspin*
  protocol (FLAC) and plays through libmpv, on your LAN or over the remote data channel.
- **Full transport** — play/pause, next/previous, seek, **shuffle**, and **repeat**
  (off / one / all).
- **Background music** — leave the player screen to keep browsing and queueing while audio
  keeps playing.

### Queue
- A **side-sheet queue** with a *Now Playing* header and an *Up Next* list.
- **Reorder** tracks (bumper buttons, a grab-and-move mode, or touch drag-and-drop),
  **remove** tracks (button or swipe), **jump** to any track, and **clear** what's up next.
- Every queue action goes through the Music Assistant server (`player_queues/*`), so the
  Vita and the rest of your setup always agree; the sheet refreshes live on queue events.

### Context menus
- Press **START** on any artist, album, track, or playlist for a compact options popover:
  Play Now, Play Next, Add to Queue, Add to Playlist, Shuffle/Play All (artists), and
  playlist management.

### Settings
- **Theme**, debug logging, and an optional Debug tab.
- **Player Name** shown in Music Assistant, and the **selected player** to control.
- **Play audio on this Vita** toggle (use the Vita purely as a remote control if you like).
- **Remote access** — set/paste the Remote ID and **Connect Now**.
- **Seek interval**, **controls auto-hide**, **default track action**, **stream quality**,
  and **connection timeout**.
- **Sidebar reordering**, collapsing empty sections, and hiding titles.
- **Network test** and **local playback test** helpers.

---

## Installing

1. Download **`VitaMusicAssistant.vpk`** from the
   [`build-output`](../../tree/build-output) branch.
2. Install it with **VitaShell** (or your preferred installer) on a homebrew-enabled Vita.
3. Launch **Vita Music Assistant** from the LiveArea.

> **First run:** sign in with your Music Assistant server URL and credentials once. To use
> **Remote** access afterwards, grab your server's Remote ID (Settings → Remote Access in
> Music Assistant) — type it, paste a link, or scan its QR code.

---

## Building

Built entirely in CI (see [`.github/workflows/build-vpk.yml`](.github/workflows/build-vpk.yml)),
which produces the `.vpk` on every push. To build locally you need the
[vitasdk](https://vitasdk.org/) toolchain and the switchfin prebuilt packages the workflow
installs (`mbedtls`, `curl`, `ffmpeg`, `mpv`), then:

```sh
cmake -B build -G Ninja
cmake --build build
```

The build vendors and patches several libraries for the Vita — libdatachannel and its deps,
mbedTLS (rebuilt with DTLS-SRTP), curl, and the [quirc](https://github.com/dlbeer/quirc) QR
decoder. See [`patches/`](patches/) and `CMakeLists.txt` for details.

---

## Tech stack

| Area            | Library |
|-----------------|---------|
| UI              | Borealis (GXM renderer) |
| Audio playback  | libmpv + FFmpeg |
| Remote access   | libdatachannel (libjuice, usrsctp) over mbedTLS DTLS |
| TLS / HTTP      | mbedTLS, curl |
| QR decoding     | quirc |
| Camera          | `sceCamera` |

---

## Acknowledgements

- The [Music Assistant](https://github.com/music-assistant) project and its server, mobile,
  and frontend teams.
- [vitasdk](https://vitasdk.org/), [Borealis](https://github.com/natinusala/borealis), and
  every vendored library above.
