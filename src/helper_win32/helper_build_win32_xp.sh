#!/bin/bash
# Build the Win32 bridge (anetmrc_bridge.exe) and config utility (config.exe)
# targeting Windows XP SP3 (PE subsystem 5.01).
#
# Differences vs helper_build_win32.sh:
#   - Defines ANETMRC_XP_BUILD to activate the GetTickCount64 shim in
#     helper_protocol.c (GetTickCount64 is Vista+).
#   - Sets _WIN32_WINNT/WINVER to 0x0501 so headers expose only XP-era APIs.
#   - Passes --major-subsystem-version 5 --minor-subsystem-version 1 so the
#     XP loader will accept the resulting PE (mingw-w64 defaults to 6.0).
#
# TLS note: XP SP3's SChannel only negotiates up to TLS 1.0. The bridge's
# AcquireCredentialsHandle call requests TLS 1.0/1.1/1.2 — XP will silently
# fall back to TLS 1.0. If the MRC server refuses TLS 1.0, this build cannot
# connect. That's an OS crypto-stack limitation, not a build issue.

set -euo pipefail

OUTDIR="build_helper_win32_xp"
mkdir -p "$OUTDIR"

CC="${CC:-i686-w64-mingw32-gcc}"

XP_DEFINES="-DANETMRC_XP_BUILD=1 -D_WIN32_WINNT=0x0501 -DWINVER=0x0501"
XP_LDFLAGS="-Wl,--major-subsystem-version,5 -Wl,--minor-subsystem-version,1 -Wl,--major-os-version,5 -Wl,--minor-os-version,1"

$CC -O2 -Wall -Wextra $XP_DEFINES -DSECURITY_WIN32 \
  -o "$OUTDIR/anetmrc_bridge.exe" \
  helper_main.c helper_protocol.c \
  $XP_LDFLAGS \
  -lws2_32 -lsecur32 -lcrypt32

echo "Built: $OUTDIR/anetmrc_bridge.exe (XP SP3 target)"

$CC -O2 -Wall -Wextra $XP_DEFINES \
  -o "$OUTDIR/config.exe" \
  config_main.c \
  $XP_LDFLAGS

echo "Built: $OUTDIR/config.exe (XP SP3 target)"

# Build the native Win32 local client for XP (anetmrc_l.exe).
#   - Shares main.c + bridge.c with the DOS door.
#   - -DANETMRC_XP_BUILD suppresses SetCurrentConsoleFontEx (Vista+ API) so
#     the exe links against kernel32 imports XP actually exports.
#   - -DANETMRC_LOCAL_ONLY forces local mode, skips dropfile parsing.
$CC -O2 -Wall -Wextra $XP_DEFINES -DANETMRC_LOCAL_ONLY \
  -I ../dosdoor \
  -o "$OUTDIR/anetmrc_l.exe" \
  ../dosdoor/main.c ../dosdoor/bridge.c fossil_win32.c \
  $XP_LDFLAGS

echo "Built: $OUTDIR/anetmrc_l.exe (XP SP3 target)"
