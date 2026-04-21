# Changelog

## [1.3.9] - 2026-04-21

### Added
- **Native Win32 local client (`anetmrc_l.exe`).**  The DOS door is a
  16-bit MZ executable, so `anetmrc.exe --local` no longer runs on any
  64-bit Windows.  The new `anetmrc_l.exe` is a native Win32 console
  application (statically linked, no DLL dependencies beyond what
  Windows ships) that reuses `dosdoor/main.c` and `dosdoor/bridge.c`
  via `-DANETMRC_LOCAL_ONLY`, and substitutes a new
  `helper_win32/fossil_win32.c` for the INT&nbsp;14h FOSSIL driver.
  Uses bridge slot `ANETDOS0.OUT/.IN` so it does not collide with a
  live node&nbsp;1.  Ships in both the regular and XP packages.
- **Platform shim in `dosdoor/common.h`.**  `anet_delay()` and
  `anet_idle_hint()` abstract `delay()` / `INT&nbsp;28h` (DOS) vs
  `Sleep()` / no-op (Win32), letting `main.c` compile under both
  OpenWatcom 2.0 and mingw-w64 i686.

### Changed
- **Double-buffered rendering in `anetmrc_l.exe`.**  All output goes
  through an off-screen 80&times;25 `CHAR_INFO` framebuffer.  The ANSI
  escape interpreter writes cursor moves, SGR changes, and erases into
  the buffer locally; each `fossil_puts` / `fossil_printf` /
  `fossil_cls` call ends with one atomic `WriteConsoleOutputA` of the
  dirty row range.  No flicker during input redraws, even on Windows 7
  conhost.  Dirty-row tracking keeps small updates (one-line input
  rewrites on every keystroke) to a single-row blit.
- **`ENABLE_VIRTUAL_TERMINAL_PROCESSING` not required.**  The interpreter
  handles ANSI SGR/CUP/ED/EL escapes directly via
  `SetConsoleTextAttribute` / `SetConsoleCursorPosition` /
  `FillConsoleOutput*`, so the client renders correctly on Windows XP
  through 11 without relying on the Windows&nbsp;10 1511+ VT mode.
- **Forced console font (Lucida Console 8&times;16) and locked window
  chrome.**  `SetCurrentConsoleFontEx` pins the font on Vista+ for a
  predictable look; `SetWindowLong` strips `WS_MAXIMIZEBOX` and
  `WS_THICKFRAME` so the window can't be accidentally resized, and
  `DeleteMenu` on the system menu drops `SC_CLOSE`/`SC_SIZE`/
  `SC_MAXIMIZE`.  Original style, buffer size, cursor visibility, code
  page, and window title are all saved at init and restored on exit so
  the parent shell is left exactly as it was.
- **Cursor hidden in `anetmrc_l.exe`.**  The DOS door doesn't rely on
  a visible cursor; hiding it kills the caret-blink flicker during
  rapid input redraws on legacy conhost.
- **Screen buffer collapsed to 80&times;25.**  Removes the scrollbar
  and the empty dead zone below the content.
- **DOS door scrollback raised to 250 lines (was 100).**  `/bbses`
  returns 170+ rows on a populated network; users could no longer
  scroll to the top.  `MSG_MAX` is 250, `MSG_LEN` 160 (was 200).
  `g_msgs[250][160] = 40&nbsp;KB` is auto-placed by Watcom's compact
  model in the far-data segment `main13_DATA`, so DGROUP stays well
  under the 64&nbsp;KB limit (~45&nbsp;KB used).
- Version strings bumped to `1.3.9`.


## [1.3.8-xp] - 2026-04-20

### Added
- **Windows XP SP3 build (`anetmrc_v1.3.8_xp.zip`).**  Parallel build
  target for users still running XP SP3.  No source changes from 1.3.8
  beyond a compile-time shim: `helper_protocol.c` gains an
  `ANETMRC_XP_BUILD`-gated wrapper around `GetTickCount64` (Vista+) that
  tracks 49.7-day wraps using plain `GetTickCount`, and
  `helper_build_win32_xp.sh` passes `_WIN32_WINNT=0x0501` /
  `--major-subsystem-version 5 --minor-subsystem-version 1` so the
  resulting PE loads on XP.  The DOS door (`anetmrc.exe`) is unchanged
  — 16-bit DOS has no Windows version dependency.
- **`README_XP.TXT`** packaged at the root of the XP zip documents the
  differences and the TLS caveat (XP SP3's SChannel only negotiates
  TLS&nbsp;1.0; if the MRC server refuses that, the build can't connect
  — an OS crypto-stack limit, not a bridge bug).


## [1.3.8] - 2026-04-18

### Fixed
- **`--local` colliding with COM1/node 1.**  `--local` was hardcoded to
  `comm_port = 0`, which resolved to `ANETDOS.OUT/IN` — the same files
  COM1 uses.  Running `--local` alongside a live node 1 made the two
  doors share one bridge slot, so each could see the other's traffic
  (including connection state, identify replies, and chat).  `--local`
  now uses a dedicated `ANETDOS0.OUT/IN` pair, and the bridge reuses
  slot 1 (previously dormant) to service it.  The `[N<slot>]` prefix
  has been added to every `helper_log` line to make multi-node logs
  unambiguous.

### Changed
- **Unknown `/command` is forwarded to the MRC server.**  Previously,
  any `/` command the door did not recognise produced
  `ERR Unknown: /foo (try /help)`.  With new server-side helpers being
  added regularly (e.g. `!list`, `!welcome`, `!games`), the door now
  converts unknown `/foo args` to `!foo args` and sends it through the
  bridge; the helper's unknown-`!`-command catch-all already forwards
  to the server as a room message and captures the response via WHOON
  routing.  If the server doesn't know the command, it returns its own
  error.  No client updates needed as the MRC helper command set grows.
- Version strings bumped to `1.3.8`.


## [1.3.7] - 2026-04-17

### Fixed
- **CTCP reply leak to non-target users.**  The MRC relay broadcasts every
  `ctcp_echo_channel` packet to every client listening on that pseudo-room,
  and each client is responsible for filtering by field&nbsp;4 (`to_user`).
  The `[CTCP-REPLY]` display path in `handle_mrc_packet` never enforced
  this gate, so any user on ANETMRC would see CTCP replies directed at
  somebody else on the network.  The fix adds a `to_user == my_nick` check
  before `bridge_write` for both the SERVER-sourced branch and the
  user-to-user branch in `helper_protocol.c`.  The `[CTCP]` request path
  was already correctly gated on `ctcp_target`, so that direction is
  unchanged.

### Changed
- Version strings bumped to `1.3.7` in `helper_common.h`
  (`ANETMRC_VERSION_INFO` sent in the handshake and in CTCP VERSION
  replies), the help-screen banner in `dosdoor/main.c`, and the top-of-file
  banners in `README.TXT`, `INSTALL.TXT`, and `FILE_ID.DIZ`.


## [1.3.6] - 2026-04-16

### Fixed
- **`/chatters` and `/list` mangled join/part notices and `!games` output.**
  When the `chatters_notme_count` or `rooms_notme_count` window was still
  open from a prior `/chatters` / `/list`, unrelated server content
  (join/part broadcasts, `!games` response tables, flags-legend footers)
  was being pushed through the column-extract formatters and losing
  characters.  Added `is_list_flags_legend()` and `looks_like_chatters_row()`
  detectors in `helper_protocol.c`; non-data rows now pass through
  `passthrough_truncate()` (which preserves original spacing up to
  78&nbsp;visible columns) instead of going through the split formatters.
- **Auto-MOTD mis-firing on every MOTD line.**  The trigger previously
  matched any body containing the substring "Welcome to", which the real
  MOTD includes multiple times.  Gate tightened to require both
  "Welcome to" **and** "the room is" so it only matches the genuine
  post-identify join banner.

### Changed
- Version strings bumped to `1.3.6`.


## [1.3.5] - 2026-04-15

Initial packaged release of ANETMRC on the current build infrastructure.
Baseline functionality:

- 16-bit DOS door (`anetmrc.exe`) built with OpenWatcom, compact memory
  model, Pentium target.  Uses FOSSIL serial I/O, reads `DOOR.SYS` for
  COM port and user identity, falls back to `--local` (stdin/stdout) for
  DOSEMU testing.
- Win32 bridge (`anetmrc_bridge.exe`) built with mingw-w64 i686.
  Maintains the TCP connection to an MRC relay
  (`na-multi.relaychat.net:5000` by default) and translates between the
  7-field tilde-separated MRC protocol and the door's pipe-coded
  (`|NN`) file bridge.
- One-time configuration utility (`config.exe`) that writes
  `MRCBBS.DAT` (BBS-wide identity: `bbs_name`, `bbs_pretty`, `sysop`,
  `description`, `telnet`, `ssh`, `website`, MRC server/port,
  `show_motd`).  Per-user state (`handle`, `handle_color`,
  prefix/suffix, `text_color`, enter/leave room messages, theme,
  twit list) is written to `MRCUSER.DAT` by the door.
- MRC 1.3 protocol: handshake, `IAMHERE` every 60&nbsp;s,
  `IMALIVE` only in response to server `PING`, `CAPABILITIES`,
  `USERIP`, `TERMSIZE`, `BBSMETA`, `INFODSC/INFOTEL/INFOSSH/INFOWEB/
  INFOSYS` sent on connect.
- Chat features: `/join`, `/list`, `/chatters`, `/whoon`, `/msg`
  (+ `/t`, `/r` shortcuts), `/me`, `/b` broadcast, `/afk` / `/back`,
  `/mentions`, `/twit` / `/untwit`, `/color`, `/prefix`, `/suffix`,
  `/theme`.  Server commands: `/motd`, `/time`, `/version`, `/stats`,
  `/banners`, `/users`, `/channel`, `/bbses`, `/info N`, `/topic`,
  `/last`, `/lastseen`, `/topics`, `/routing`, `/changelog`,
  `/roompass`, `/roomconfig`, `/helpserver`, `!<command>` for any
  server helper.
- MRC Trust: `/identify`, `/register`, `/update`, `/trust`.
- CTCP (VERSION, TIME, PING, CLIENTINFO) via `ctcp_echo_channel`,
  both request and reply direction.
- Multi-node single-bridge architecture: `node_state_t g_nodes[9]`
  with `g_cur_node` pointer swapping; one bridge process polls every
  `ANETDOS*.OUT` in its CWD and activates node slots on demand.
  Per-node bridge file pairs `ANETDOS[N].OUT/.IN` keyed off the door's
  COM port.
- Scrollback: 100-line ring with pre-wrapped entries, PgUp/PgDn/Home/
  End navigation, new-messages-while-scrolled counter, auto-resume on
  ESC or Enter.
- Mentions: starred lines in the scrollback, pageable `/mentions` log
  (`MRCMENT.LOG`), on-screen counter.  Twit filter suppresses messages
  from handles on the per-user twit list.
- Tab-complete handles from the current room's user list.
- 80&times;24 ANSI screen layout, 6 border themes (gray / blue /
  green / cyan / red / magenta), 15 handle colors, 15 text colors,
  optional prefix/suffix decoration around the handle.
- Password masking for `/identify`, `/register`, `/roompass`,
  `/update password`.  Up-arrow sent-message recall suppresses
  password-bearing commands from history.


## Notes

[1.3.9]:    https://github.com/YOURNAME/anetmrc/releases/tag/v1.3.9
[1.3.8-xp]: https://github.com/YOURNAME/anetmrc/releases/tag/v1.3.8-xp
[1.3.8]:    https://github.com/YOURNAME/anetmrc/releases/tag/v1.3.8
[1.3.7]:    https://github.com/YOURNAME/anetmrc/releases/tag/v1.3.7
[1.3.6]:    https://github.com/YOURNAME/anetmrc/releases/tag/v1.3.6
[1.3.5]:    https://github.com/YOURNAME/anetmrc/releases/tag/v1.3.5
