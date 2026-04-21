/* fossil_win32.c — Win32 console replacement for fossil.c (local mode only).
 *
 * Used by anetmrc_l.exe, the native Windows local client. It satisfies the
 * same fossil.h API the DOS door calls into, but routes all I/O through
 * the Win32 console instead of INT 14h FOSSIL.
 *
 * Rendering strategy: double-buffered.
 *   - An 80x25 CHAR_INFO framebuffer sits off-screen. The ANSI escape
 *     interpreter writes into the framebuffer — cursor moves, SGR, erases
 *     are all local operations, never visible.
 *   - Every fossil_puts / fossil_printf / fossil_cls call ends with one
 *     atomic WriteConsoleOutputA that blits the dirty row range onto the
 *     real console. That is the only Win32 API call that paints visible
 *     pixels. Because the console compositor sees one whole-rectangle
 *     update (not dozens of FillConsoleOutput/cursor-move sequences),
 *     there is no flicker — even on Windows 7 conhost.
 *   - A dirty row range (g_dirty_top..g_dirty_bot) means small redraws
 *     (like the one-line input rewrite on every keystroke) blit only the
 *     row that actually changed. Full-screen redraws blit all 25 rows.
 *
 * Other bits:
 *   - Console output code page forced to 437 so CP437 block chars render.
 *   - Cursor hidden (door doesn't need one; hiding kills Win7's blinking-
 *     caret flicker during redraws).
 *   - Screen buffer collapsed to exactly 80x25 so there's no scrollbar
 *     and no empty rows under the content.
 *   - Window is non-resizable, non-maximizable.
 *   - Console font forced to Lucida Console 8x16 (Vista+ API).
 *   - fossil_getch_nonblock synthesizes the BIOS scancode pairs main.c
 *     expects (PgUp=73, Up=72, etc.) from ReadConsoleInputW events.
 */

/* Vista+ target unless ANETMRC_XP_BUILD is set — in which case we stay at
 * _WIN32_WINNT=0x0501 (set on the command line by helper_build_win32_xp.sh)
 * and skip the one Vista-only call (SetCurrentConsoleFontEx). */
#if !defined(ANETMRC_XP_BUILD)
#  if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#    undef  _WIN32_WINNT
#    define _WIN32_WINNT 0x0600
#  endif
#endif

#include <windows.h>
#include <wincon.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include "../dosdoor/fossil.h"

/* Framebuffer dimensions — matches the DOS door's fixed 80x24 layout plus
 * one cushion row so row-24 writes don't wrap unexpectedly. */
#define FB_W 80
#define FB_H 25

/* Default attribute = light gray on black — matches the door's "|07" reset. */
#define DEFAULT_ATTR ((WORD)(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE))

/* Console state saved at init, restored at deinit. */
static HANDLE g_hin  = INVALID_HANDLE_VALUE;
static HANDLE g_hout = INVALID_HANDLE_VALUE;
static DWORD  g_in_mode_saved  = 0;
static DWORD  g_out_mode_saved = 0;
static UINT   g_out_cp_saved   = 0;
static UINT   g_in_cp_saved    = 0;
static WORD   g_attr_saved     = DEFAULT_ATTR;
static COORD  g_bufsize_saved  = { 0, 0 };
static SMALL_RECT g_win_saved  = { 0, 0, 0, 0 };
static CONSOLE_CURSOR_INFO g_cursor_saved = { 25, TRUE };
static CHAR   g_title_saved[256] = { 0 };
static LONG   g_wstyle_saved   = 0;
static int    g_wstyle_saved_ok = 0;
static int    g_initialized    = 0;
static int    g_pending        = -1; /* second byte of an extended key */

/* Framebuffer + render state. */
static CHAR_INFO g_fb[FB_W * FB_H];
static SHORT g_cur_x = 0, g_cur_y = 0;
static WORD  g_attrs = DEFAULT_ATTR;
static int   g_dirty_top = -1;       /* -1 = no dirty rows pending */
static int   g_dirty_bot = -1;
static SHORT g_origin_x = 0;         /* real console coords of framebuffer */
static SHORT g_origin_y = 0;

/* ANSI parser state machine. */
typedef enum {
    ANSI_NORMAL = 0,
    ANSI_ESC    = 1,
    ANSI_CSI    = 2
} ansi_state_t;

static ansi_state_t g_state = ANSI_NORMAL;
static int g_params[8];
static int g_nparams = 0;
static int g_have_param = 0;

/* Map a Win32 VK_* code to its BIOS extended scancode. 0 = swallow. */
static int vk_to_scan(WORD vk) {
    switch (vk) {
        case VK_UP:    return 72;
        case VK_DOWN:  return 80;
        case VK_LEFT:  return 75;
        case VK_RIGHT: return 77;
        case VK_HOME:  return 71;
        case VK_END:   return 79;
        case VK_PRIOR: return 73; /* PgUp */
        case VK_NEXT:  return 81; /* PgDn */
        case VK_F1:    return 59;
        case VK_F2:    return 60;
        case VK_F3:    return 61;
        case VK_F4:    return 62;
        case VK_F5:    return 63;
        case VK_F6:    return 64;
        case VK_F7:    return 65;
        case VK_F8:    return 66;
        case VK_F9:    return 67;
        case VK_F10:   return 68;
        case VK_DELETE: return 83;
        case VK_INSERT: return 82;
        default: return 0;
    }
}

/* ---------------------------------------------------------------------------
 * Framebuffer primitives
 * ------------------------------------------------------------------------- */

static void fb_mark_dirty(int row) {
    if (row < 0 || row >= FB_H) return;
    if (g_dirty_top < 0 || row < g_dirty_top) g_dirty_top = row;
    if (g_dirty_bot < 0 || row > g_dirty_bot) g_dirty_bot = row;
}

static void fb_init(void) {
    int i;
    for (i = 0; i < FB_W * FB_H; ++i) {
        g_fb[i].Char.AsciiChar = ' ';
        g_fb[i].Attributes     = DEFAULT_ATTR;
    }
    g_cur_x = 0; g_cur_y = 0;
    g_dirty_top = 0; g_dirty_bot = FB_H - 1;
}

/* Write one non-escape byte into the framebuffer at the cursor. */
static void fb_putc(unsigned char c) {
    int idx;
    if (c == '\r') { g_cur_x = 0; return; }
    if (c == '\n') {
        g_cur_y++;
        if (g_cur_y >= FB_H) g_cur_y = FB_H - 1;
        return;
    }
    if (c == 0x08) { /* backspace */
        if (g_cur_x > 0) g_cur_x--;
        return;
    }
    if (c == '\t') {
        /* Rare in the door's output; 8-col tabs. */
        int next = (g_cur_x + 8) & ~7;
        while (g_cur_x < next && g_cur_x < FB_W) {
            idx = g_cur_y * FB_W + g_cur_x;
            g_fb[idx].Char.AsciiChar = ' ';
            g_fb[idx].Attributes     = g_attrs;
            g_cur_x++;
        }
        fb_mark_dirty(g_cur_y);
        return;
    }
    if (g_cur_x >= FB_W) {
        /* Auto-wrap: the door rarely runs off the right edge, but handle it. */
        g_cur_x = 0;
        g_cur_y++;
        if (g_cur_y >= FB_H) g_cur_y = FB_H - 1;
    }
    if (g_cur_y < 0 || g_cur_y >= FB_H) return;
    idx = g_cur_y * FB_W + g_cur_x;
    g_fb[idx].Char.AsciiChar = (CHAR)c;
    g_fb[idx].Attributes     = g_attrs;
    fb_mark_dirty(g_cur_y);
    g_cur_x++;
}

/* Move cursor to 1-based row/col (window-relative). */
static void fb_move(int row, int col) {
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    g_cur_y = (SHORT)(row - 1);
    g_cur_x = (SHORT)(col - 1);
    if (g_cur_x >= FB_W) g_cur_x = FB_W - 1;
    if (g_cur_y >= FB_H) g_cur_y = FB_H - 1;
}

/* Erase In Display, param 2 = entire window. */
static void fb_clear(void) {
    int i;
    for (i = 0; i < FB_W * FB_H; ++i) {
        g_fb[i].Char.AsciiChar = ' ';
        g_fb[i].Attributes     = g_attrs;
    }
    g_cur_x = 0; g_cur_y = 0;
    g_dirty_top = 0;
    g_dirty_bot = FB_H - 1;
}

/* Erase In Line, param 0 = cursor to EOL. */
static void fb_clear_eol(void) {
    int x;
    if (g_cur_y < 0 || g_cur_y >= FB_H) return;
    for (x = g_cur_x; x < FB_W; ++x) {
        int idx = g_cur_y * FB_W + x;
        g_fb[idx].Char.AsciiChar = ' ';
        g_fb[idx].Attributes     = g_attrs;
    }
    fb_mark_dirty(g_cur_y);
}

/* Blit the dirty rows to the real console. Atomic from the compositor's
 * point of view — no flicker regardless of how many cells changed. */
static void fb_flush(void) {
    COORD bufsize, bufcoord;
    SMALL_RECT region;
    if (!g_initialized) return;
    if (g_dirty_top < 0 || g_dirty_bot < 0) return;
    /* WriteConsoleOutputA needs the source rect — pass the full 80-wide
     * band covering the dirty row range. */
    bufsize.X = FB_W;
    bufsize.Y = (SHORT)(g_dirty_bot - g_dirty_top + 1);
    bufcoord.X = 0;
    bufcoord.Y = (SHORT)g_dirty_top;
    region.Left   = g_origin_x;
    region.Top    = (SHORT)(g_origin_y + g_dirty_top);
    region.Right  = (SHORT)(g_origin_x + FB_W - 1);
    region.Bottom = (SHORT)(g_origin_y + g_dirty_bot);
    /* WriteConsoleOutputA reads a rectangle out of a buffer the size of
     * (bufsize). Trick: pass g_fb as if it were bufsize tall, with the
     * coord offset pointing into it — BUT the API expects the buffer
     * dimensions to match the source pointer layout. To keep it simple,
     * pass the whole 80xFB_H layout and a bufcoord that skips the clean
     * rows at the top. */
    bufsize.X = FB_W;
    bufsize.Y = FB_H;
    bufcoord.X = 0;
    bufcoord.Y = (SHORT)g_dirty_top;
    WriteConsoleOutputA(g_hout, g_fb, bufsize, bufcoord, &region);
    g_dirty_top = -1;
    g_dirty_bot = -1;
}

/* ---------------------------------------------------------------------------
 * ANSI SGR → Win32 attribute mapping
 * ------------------------------------------------------------------------- */

static void sgr_apply(int p) {
    static const WORD fg_tab[8] = {
        0,
        FOREGROUND_RED,
        FOREGROUND_GREEN,
        FOREGROUND_RED | FOREGROUND_GREEN,
        FOREGROUND_BLUE,
        FOREGROUND_RED | FOREGROUND_BLUE,
        FOREGROUND_GREEN | FOREGROUND_BLUE,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE
    };
    static const WORD bg_tab[8] = {
        0,
        BACKGROUND_RED,
        BACKGROUND_GREEN,
        BACKGROUND_RED | BACKGROUND_GREEN,
        BACKGROUND_BLUE,
        BACKGROUND_RED | BACKGROUND_BLUE,
        BACKGROUND_GREEN | BACKGROUND_BLUE,
        BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE
    };

    if (p == 0) {
        g_attrs = DEFAULT_ATTR;
    } else if (p == 1) {
        g_attrs |= FOREGROUND_INTENSITY;
    } else if (p == 22) {
        g_attrs &= (WORD)~FOREGROUND_INTENSITY;
    } else if (p >= 30 && p <= 37) {
        WORD fg_mask = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        g_attrs = (WORD)((g_attrs & ~fg_mask) | fg_tab[p - 30]);
    } else if (p >= 90 && p <= 97) {
        WORD fg_mask = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        g_attrs = (WORD)((g_attrs & ~fg_mask) | fg_tab[p - 90] |
                         FOREGROUND_INTENSITY);
    } else if (p >= 40 && p <= 47) {
        WORD bg_mask = BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;
        g_attrs = (WORD)((g_attrs & ~bg_mask) | bg_tab[p - 40]);
    } else if (p >= 100 && p <= 107) {
        WORD bg_mask = BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE;
        g_attrs = (WORD)((g_attrs & ~bg_mask) | bg_tab[p - 100] |
                         BACKGROUND_INTENSITY);
    }
    /* Unknown params silently ignored. */
}

/* Dispatch on the final byte of a CSI sequence. */
static void csi_dispatch(char final) {
    switch (final) {
        case 'm': {
            int i;
            if (g_nparams == 0) sgr_apply(0);
            for (i = 0; i < g_nparams; ++i) sgr_apply(g_params[i]);
            break;
        }
        case 'H': case 'f': {
            int r = g_nparams > 0 ? g_params[0] : 1;
            int c = g_nparams > 1 ? g_params[1] : 1;
            fb_move(r, c);
            break;
        }
        case 'J': {
            int p = g_nparams > 0 ? g_params[0] : 0;
            if (p == 2) fb_clear();
            break;
        }
        case 'K': {
            int p = g_nparams > 0 ? g_params[0] : 0;
            if (p == 0) fb_clear_eol();
            break;
        }
        default: break;
    }
}

/* Feed one byte through the ANSI state machine. */
static void ansi_feed(unsigned char b) {
    switch (g_state) {
        case ANSI_NORMAL:
            if (b == 0x1B) { g_state = ANSI_ESC; break; }
            fb_putc(b);
            break;

        case ANSI_ESC:
            if (b == '[') {
                g_state = ANSI_CSI;
                g_nparams = 0;
                g_have_param = 0;
                memset(g_params, 0, sizeof(g_params));
            } else {
                /* Lone ESC or ESC-other: treat as literal. */
                fb_putc(0x1B);
                fb_putc(b);
                g_state = ANSI_NORMAL;
            }
            break;

        case ANSI_CSI:
            if (b >= '0' && b <= '9') {
                if (g_nparams < 8) {
                    g_params[g_nparams] = g_params[g_nparams] * 10 + (b - '0');
                    g_have_param = 1;
                }
            } else if (b == ';') {
                if (g_have_param && g_nparams < 7) g_nparams++;
                g_have_param = 0;
                if (g_nparams < 8) g_params[g_nparams] = 0;
            } else if (b >= 0x40 && b <= 0x7E) {
                if (g_have_param && g_nparams < 8) g_nparams++;
                g_have_param = 0;
                csi_dispatch((char)b);
                g_state = ANSI_NORMAL;
            }
            break;
    }
}

/* ---------------------------------------------------------------------------
 * fossil.h API
 * ------------------------------------------------------------------------- */

void fossil_set_port(unsigned short port) { (void)port; }
void fossil_set_local_mode(void) { }

/* Force the console font to a predictable VGA-ish look. Vista+ API; on the
 * XP build this is a no-op and the user's registered default font applies. */
static void set_console_font(void) {
#ifdef ANETMRC_XP_BUILD
    /* SetCurrentConsoleFontEx is Vista+. On XP we keep the default font;
     * Raster Fonts renders CP437 block chars correctly anyway. */
#else
    CONSOLE_FONT_INFOEX cfi;
    memset(&cfi, 0, sizeof(cfi));
    cfi.cbSize       = sizeof(cfi);
    cfi.nFont        = 0;
    cfi.dwFontSize.X = 8;
    cfi.dwFontSize.Y = 16;
    cfi.FontFamily   = FF_DONTCARE;
    cfi.FontWeight   = FW_NORMAL;
    wcsncpy(cfi.FaceName, L"Lucida Console", LF_FACESIZE - 1);
    SetCurrentConsoleFontEx(g_hout, FALSE, &cfi);
#endif
}

/* Strip window chrome bits that let the user accidentally resize/close. */
static void lock_window_chrome(void) {
    HWND hwnd = GetConsoleWindow();
    if (!hwnd) return;
    g_wstyle_saved    = GetWindowLongA(hwnd, GWL_STYLE);
    g_wstyle_saved_ok = 1;
    SetWindowLongA(hwnd, GWL_STYLE,
                   g_wstyle_saved & ~(LONG)(WS_MAXIMIZEBOX | WS_THICKFRAME));
    /* Remove the close item from the system menu. Keeps the red X grayed
     * out (actually dims/removes depending on Windows version). Users can
     * still exit cleanly via /quit or Esc. */
    {
        HMENU sysm = GetSystemMenu(hwnd, FALSE);
        if (sysm) {
            DeleteMenu(sysm, SC_CLOSE,    MF_BYCOMMAND);
            DeleteMenu(sysm, SC_SIZE,     MF_BYCOMMAND);
            DeleteMenu(sysm, SC_MAXIMIZE, MF_BYCOMMAND);
        }
    }
    /* Force the window manager to redraw the frame with the new style. */
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

/* Restore window chrome on exit so the parent shell isn't left with a
 * locked-down frame. */
static void unlock_window_chrome(void) {
    HWND hwnd;
    if (!g_wstyle_saved_ok) return;
    hwnd = GetConsoleWindow();
    if (!hwnd) return;
    SetWindowLongA(hwnd, GWL_STYLE, g_wstyle_saved);
    SetWindowPos(hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    /* System menu items can't be re-added cleanly; the WM restores them
     * when the window is closed, which is fine for our use case. */
}

int fossil_init(void) {
    if (g_initialized) return 1;

    g_hin  = GetStdHandle(STD_INPUT_HANDLE);
    g_hout = GetStdHandle(STD_OUTPUT_HANDLE);
    if (g_hin == INVALID_HANDLE_VALUE || g_hout == INVALID_HANDLE_VALUE)
        return 0;

    if (GetConsoleMode(g_hin, &g_in_mode_saved)) {
        DWORD m = g_in_mode_saved;
        m &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                      ENABLE_PROCESSED_INPUT);
        m |=  (DWORD)(ENABLE_WINDOW_INPUT);
        SetConsoleMode(g_hin, m);
    }
    (void)GetConsoleMode(g_hout, &g_out_mode_saved);

    {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(g_hout, &csbi)) {
            g_attr_saved    = csbi.wAttributes;
            g_bufsize_saved = csbi.dwSize;
            g_win_saved     = csbi.srWindow;
        }
    }
    GetConsoleCursorInfo(g_hout, &g_cursor_saved);
    GetConsoleTitleA(g_title_saved, sizeof(g_title_saved));

    /* CP437 so block chars render. */
    g_out_cp_saved = GetConsoleOutputCP();
    g_in_cp_saved  = GetConsoleCP();
    SetConsoleOutputCP(437);
    SetConsoleCP(437);

    /* Force font before resizing so the 80x25 window sits at a predictable
     * pixel size. */
    set_console_font();

    /* Collapse buffer + window to exactly 80x25 — no scrollbar, no empty
     * rows below the content. Shrink window first (window is always <=
     * buffer), then resize buffer, then apply window again. */
    {
        SMALL_RECT win;
        COORD size;
        win.Left = 0; win.Top = 0;
        win.Right = 79; win.Bottom = 24;
        SetConsoleWindowInfo(g_hout, TRUE, &win);
        size.X = FB_W; size.Y = FB_H;
        SetConsoleScreenBufferSize(g_hout, size);
        SetConsoleWindowInfo(g_hout, TRUE, &win);
    }

    /* Lock chrome AFTER geometry is set, so the redraw picks up the new
     * style with the final size. */
    lock_window_chrome();

    /* Hide cursor. */
    {
        CONSOLE_CURSOR_INFO ci;
        ci.dwSize   = 25;
        ci.bVisible = FALSE;
        SetConsoleCursorInfo(g_hout, &ci);
    }

    SetConsoleTitleA("ANETMRC Local Client");

    /* Remember where the framebuffer maps onto the real screen buffer —
     * matters if the user or an earlier program left the window offset. */
    {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(g_hout, &csbi)) {
            g_origin_x = csbi.srWindow.Left;
            g_origin_y = csbi.srWindow.Top;
        }
    }

    g_attrs = DEFAULT_ATTR;
    fb_init();
    fb_flush();

    g_initialized = 1;
    return 1;
}

void fossil_deinit(void) {
    if (!g_initialized) return;
    unlock_window_chrome();
    if (g_hin  != INVALID_HANDLE_VALUE) SetConsoleMode(g_hin,  g_in_mode_saved);
    if (g_hout != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_hout, g_out_mode_saved);
        SetConsoleTextAttribute(g_hout, g_attr_saved);
        SetConsoleCursorInfo(g_hout, &g_cursor_saved);
        if (g_bufsize_saved.X > 0 && g_bufsize_saved.Y > 0) {
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (GetConsoleScreenBufferInfo(g_hout, &csbi)) {
                if (g_bufsize_saved.X >= csbi.dwSize.X &&
                    g_bufsize_saved.Y >= csbi.dwSize.Y) {
                    SetConsoleScreenBufferSize(g_hout, g_bufsize_saved);
                    SetConsoleWindowInfo(g_hout, TRUE, &g_win_saved);
                } else {
                    SetConsoleWindowInfo(g_hout, TRUE, &g_win_saved);
                    SetConsoleScreenBufferSize(g_hout, g_bufsize_saved);
                }
            }
        }
    }
    if (g_out_cp_saved) SetConsoleOutputCP(g_out_cp_saved);
    if (g_in_cp_saved)  SetConsoleCP(g_in_cp_saved);
    if (g_title_saved[0]) SetConsoleTitleA(g_title_saved);
    g_initialized = 0;
}

int fossil_getch_nonblock(void) {
    INPUT_RECORD rec;
    DWORD n = 0;

    if (!g_initialized) return -1;

    if (g_pending >= 0) {
        int c = g_pending;
        g_pending = -1;
        return c;
    }

    for (;;) {
        if (!PeekConsoleInputW(g_hin, &rec, 1, &n) || n == 0) return -1;
        if (!ReadConsoleInputW(g_hin, &rec, 1, &n) || n == 0) return -1;

        if (rec.EventType != KEY_EVENT) continue;
        if (!rec.Event.KeyEvent.bKeyDown) continue;

        {
            WORD vk = rec.Event.KeyEvent.wVirtualKeyCode;
            WCHAR wc = rec.Event.KeyEvent.uChar.UnicodeChar;

            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
                vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL)
                continue;

            {
                int scan = vk_to_scan(vk);
                if (scan) {
                    g_pending = scan;
                    return 0;
                }
            }

            if (vk == VK_RETURN) return '\r';
            if (vk == VK_BACK)   return 8;
            if (vk == VK_TAB)    return 9;
            if (vk == VK_ESCAPE) return 27;

            if (wc != 0 && wc < 256)
                return (int)(unsigned char)wc;
        }
    }
}

int fossil_getch_block(void) {
    for (;;) {
        int c = fossil_getch_nonblock();
        if (c >= 0) return c;
        WaitForSingleObject(g_hin, 50);
    }
}

/* Low-level byte output. Does NOT flush — rapid per-char writes inside
 * fossil_put_pipe (in main.c) would cause flicker if each one painted the
 * screen. Flushing is consolidated in fossil_puts / fossil_printf below. */
void fossil_putch(int ch) {
    if (!g_initialized) { putchar(ch & 0xFF); return; }
    ansi_feed((unsigned char)(ch & 0xFF));
}

/* Write a string, translating bare \n to CRLF. One atomic flush at end. */
void fossil_puts(const char *s) {
    if (!s) return;
    if (!g_initialized) { fputs(s, stdout); return; }
    while (*s) {
        if (*s == '\n') ansi_feed('\r');
        ansi_feed((unsigned char)*s++);
    }
    fb_flush();
}

void fossil_printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    fossil_puts(buf);
}

void fossil_cls(void) { fossil_puts("\033[2J\033[H"); }
