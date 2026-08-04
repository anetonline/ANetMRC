# ANetMRC — Installation Guide


*** IMPORTANT ***
* IF YOU ARE UPDATING 
* You need to run config.exe again 
* It will save your existing info, but there are also NEW options/features
* If you do not run config.exe, you will not have access to the new features



This guide walks you through:

1. Prerequisites (FOSSIL driver, Windows host)
2. Configuring the BBS identity
3. Installing ANetMRC into your BBS as a door
4. Running single-node vs. multi-node setups
5. Sanity testing

All binaries (`anetmrc.exe`, `anetmrc_bridge.exe`, `config.exe`,
`anetmrc_l.exe`) ship pre-built in the release package — no compiler or
build step is needed.

---

## 1. Prerequisites

### A FOSSIL driver on the BBS host

The door speaks serial through INT 14h via a FOSSIL driver. Any of these
work:

- **X00** — classic, widely tested
- **BNU** — small and fast (It's what I use)
- **ADF / SIO** — multi-line
- **NetFoss** — a modern FOSSIL-over-telnet wrapper for Windows-hosted
  BBSes

Install and load whichever you already use for other doors.

All `.exe` files in the release package are statically-linked Windows
console apps (no runtime DLLs needed beyond `ws2_32`, which ships with
Windows).

---

## 2. Configure the BBS identity

**Once** per BBS, run the config utility on the Windows host. It writes
`MRCBBS.DAT` into the current working directory.
(WILL NEED TO BE DONE AGAIN, FOR THE 1.4 UPDATE)

```
cd \path\to\anetmrc
config.exe
```

It will prompt for seven fields:

| Field | Example | Required? |
|---|---|---|
| BBS Display Name | `A-Net Online BBS` | Yes |
| BBS Internal Name | `ANET_ONLINE` *(auto-suggested, no spaces)* | Yes |
| SysOp Handle | `StingRay` | Yes |
| BBS Description | `Est. 1994 · fido, mrc, files` | Yes |
| Telnet Address | `a-net.online:1337` | Yes |
| SSH Address | `a-net.online:1338` | Optional |
| Website URL | `https://a-net.online` | Optional |

The file is human-readable (key=value) — you can edit it later with any
text editor or re-run `config.exe` (it loads existing values and lets you
press Enter to keep each).

If `MRCBBS.DAT` is missing or has blank required fields, the bridge
refuses to start.

---

## 3. Deploy the runtime

Create a **single directory** for the runtime. Everything below runs from
it — the bridge, the DOS door, and the MRC config — because they share
state through files in the current working directory.

Suggested layout:

```
C:\BBS\DOORS\ANetMRC\
├── anetmrc.exe              ← DOS door
├── anetmrc_bridge.exe       ← Win32 bridge
├── config.exe               ← Win32 config utility
├── MRCBBS.DAT               ← written by config.exe
├── mrc_banner.ans           ← (optional) ANSI banner for the bridge console * SIZE MATTERS!(79x15) Cannot exceed
└── *.OUT / *.IN / .LOG      ← auto-created at runtime
```

Runtime files auto-generated:

| File | Written by | What it is |
|---|---|---|
| `MRCUSER.DAT` | door | Per-user settings (handle, color, twit list, …) |
| `ANETDOS.OUT` / `ANETDOS[N].OUT` | door | Commands flowing DOS → bridge |
| `ANETDOS.IN` / `ANETDOS[N].IN` | bridge | Lines flowing bridge → DOS |
| `anetmrc_bridge.log` | bridge | Diagnostic log (only in `--verbose` mode) |

---

## 4. Launch the bridge

One bridge process serves **all** of your BBS nodes. Start it once — as a
Windows service, scheduled task, or simply in a console window.

```
anetmrc_bridge.exe
```

With diagnostic logging:

```
anetmrc_bridge.exe --verbose
```

For a legacy single-node host:

```
anetmrc_bridge.exe -node 1     # only serves COM1 / node 0-1
```

The bridge polls every `ANETDOS*.OUT` in the directory every tick. When a
new one appears (i.e., a user just launched the door on a new node), the
bridge picks it up automatically. No restart required when adding nodes.

---

## 5. Wire the door into your BBS

Add ANetMRC to your BBS's door menu using whatever mechanism your BBS
supports. It needs:

- A DOOR.SYS dropfile (standard — most BBSes emit this automatically)
- The current working directory set to the runtime folder
- The user's COM port to be online with a FOSSIL driver loaded

### Example — Spitfire `DOORS.SF` entry

```
;Name       Path                          Parms           Shell  Multi  Prompt
ANetMRC     C:\BBS\DOORS\ANetMRC          anetmrc.exe     N      Y      Y
```

### Example — Synchronet / Mystic / etc.

```
[ANetMRC]
Name       = MRC Chat
Path       = C:\BBS\DOORS\ANetMRC\
CmdLine    = anetmrc.exe --dropfile=C:\path\to\door.sys
NodeInfo   = DOOR.SYS
```

### Example — generic DOS BBS batch file

```bat
@echo off
cd C:\BBS\DOORS\ANetMRC
anetmrc.exe --dropfile=C:\path\to\door.sys
```

Whatever your BBS writes as DOOR.SYS line 1 (typically `COM1:`, `COM2:`, …) becomes the node number. 
The door reads that and picks `ANETDOS[N].OUT/.IN` automatically.

---

## 6. Sysop sanity test (no BBS required)

Run the door locally against your console to verify it talks to the
bridge correctly:

```
anetmrc.exe --local
```

This bypasses FOSSIL and reads from stdin / writes to stdout. Node 0 is
used (so `ANETDOS.OUT` / `ANETDOS.IN`). With the bridge running in
another window, you should see the handle entry screen, main menu, and be
able to connect and chat.

---

## 7. Optional: bridge-console banner

If you'd like the bridge's console window to show an ANSI banner on
startup, drop a file named `mrc_banner.ans` (CP437 ANSI art) next to
`anetmrc_bridge.exe`. Its absence is non-fatal — the bridge logs a short
note and starts normally. (do not exceed 79x15)

---

## 8. Updating

Download the new release and drop the new `.exe` files into your runtime
directory. `MRCBBS.DAT` and `MRCUSER.DAT` are preserved across upgrades.

If the protocol version changes in a future release, the bridge's
`version_info` field will say so — check `anetmrc_bridge.log` on startup.

---

## 9. Uninstall

Stop the bridge, remove the runtime directory, remove the BBS door
menu entry. Nothing is written outside the runtime folder.
