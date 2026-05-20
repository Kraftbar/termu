// Termu: tiny SecureCRT-style terminal prototype for Windows.
//
// Version: 0.2 - backend abstraction plus local Windows ConPTY backend.

#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "term_backend.h"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

#define APP_NAME L"Termu v0.2"
#define WM_BACKEND_DATA (WM_APP + 1)
#define WM_BACKEND_EXIT (WM_APP + 2)
#define TIMER_REPAINT 1
#define MAX_ROWS 80
#define MAX_COLS 160
#define MAX_HISTORY 4000
#define MAX_OSC 512
#define START_ROWS 32
#define START_COLS 100
#define DEFAULT_FG RGB(230, 230, 230)
#define DEFAULT_BG RGB(12, 16, 20)

typedef struct {
    WCHAR ch;
    COLORREF fg;
    COLORREF bg;
} Cell;

typedef enum {
    ESC_NORMAL,
    ESC_SEEN,
    ESC_CSI,
    ESC_OSC,
    ESC_OSC_ESC
} EscapeState;

static HWND g_hwnd;
static HFONT g_font;
static int g_char_w = 8;
static int g_char_h = 16;
static int g_rows = START_ROWS;
static int g_cols = START_COLS;
static int g_cx = 0;
static int g_cy = 0;
static int g_saved_cx = 0;
static int g_saved_cy = 0;
static int g_cursor_visible = 1;
static COLORREF g_cur_fg = DEFAULT_FG;
static COLORREF g_cur_bg = DEFAULT_BG;
static int g_cur_fg_index = -1;
static int g_cur_bg_index = -1;
static int g_cur_fg_bright = 0;
static int g_cur_bg_bright = 0;
static int g_bold = 0;
static int g_reverse = 0;
static Cell g_screen[MAX_ROWS][MAX_COLS];
static Cell g_history[MAX_HISTORY][MAX_COLS];
static int g_history_start = 0;
static int g_history_count = 0;
static int g_scroll_offset = 0;
static CRITICAL_SECTION g_lock;
static TermBackend g_backend;
static char g_utf8_pending[4];
static int g_utf8_pending_len = 0;
static EscapeState g_escape_state = ESC_NORMAL;
static char g_csi_buf[64];
static int g_csi_len = 0;
static WCHAR g_osc_buf[MAX_OSC];
static int g_osc_len = 0;
static HANDLE g_output_log = NULL;
static int g_repaint_pending = 0;
static int g_selecting = 0;
static int g_word_selecting = 0;
static int g_selection_active = 0;
static int g_sel_anchor_x = 0;
static int g_sel_anchor_y = 0;
static int g_sel_focus_x = 0;
static int g_sel_focus_y = 0;
static int g_word_anchor_start_x = 0;
static int g_word_anchor_end_x = 0;
static int g_word_anchor_y = 0;

static void die_box(const WCHAR* text) {
    MessageBoxW(NULL, text, APP_NAME, MB_OK | MB_ICONERROR);
}

static int backend_write(const char* s, DWORD len);
static int copy_selection_to_clipboard(void);

static void open_output_log(void) {
    g_output_log = CreateFileW(L"termu_output.log", GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_output_log == INVALID_HANDLE_VALUE) {
        g_output_log = NULL;
    }
}

static void close_output_log(void) {
    if (g_output_log) {
        CloseHandle(g_output_log);
        g_output_log = NULL;
    }
}

static void log_backend_output(const char* data, DWORD len) {
    DWORD written = 0;
    if (g_output_log && data && len > 0) {
        WriteFile(g_output_log, data, len, &written, NULL);
    }
}

static void schedule_terminal_repaint(void) {
    if (!g_hwnd) return;
    if (!g_repaint_pending) {
        g_repaint_pending = 1;
        SetTimer(g_hwnd, TIMER_REPAINT, 12, NULL);
    }
}

static void set_blank_cell(Cell* c, COLORREF fg, COLORREF bg) {
    c->ch = L' ';
    c->fg = fg;
    c->bg = bg;
}

static void clear_cell(Cell* c) {
    set_blank_cell(c, DEFAULT_FG, DEFAULT_BG);
}

static void erase_cell(Cell* c) {
    set_blank_cell(c, g_cur_fg, g_cur_bg);
}

static void clear_line(Cell* line) {
    for (int x = 0; x < MAX_COLS; x++) {
        clear_cell(&line[x]);
    }
}

static Cell* history_line_at(int index) {
    return g_history[(g_history_start + index) % MAX_HISTORY];
}

static void clear_scrollback(void) {
    g_history_start = 0;
    g_history_count = 0;
    g_scroll_offset = 0;
}

static void clamp_scroll_offset(void) {
    if (g_scroll_offset < 0) g_scroll_offset = 0;
    if (g_scroll_offset > g_history_count) g_scroll_offset = g_history_count;
}

static void push_history_line(const Cell* line) {
    int dest;
    if (g_history_count < MAX_HISTORY) {
        dest = (g_history_start + g_history_count) % MAX_HISTORY;
        g_history_count++;
    } else {
        dest = g_history_start;
        g_history_start = (g_history_start + 1) % MAX_HISTORY;
    }

    memcpy(g_history[dest], line, sizeof(Cell) * MAX_COLS);
    if (g_scroll_offset > 0) {
        g_scroll_offset++;
        clamp_scroll_offset();
    }
}

static const Cell* view_line_at(int y) {
    static Cell blank[MAX_COLS];
    static int blank_ready = 0;
    int start = g_history_count - g_scroll_offset;
    int index = start + y;

    if (!blank_ready) {
        clear_line(blank);
        blank_ready = 1;
    }

    if (index < 0) return blank;
    if (index < g_history_count) return history_line_at(index);

    index -= g_history_count;
    if (index >= 0 && index < g_rows) return g_screen[index];
    return blank;
}

static void set_scroll_offset(int offset) {
    g_scroll_offset = offset;
    clamp_scroll_offset();
    g_selection_active = 0;
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void scroll_view(int delta) {
    set_scroll_offset(g_scroll_offset + delta);
}

static void return_to_live_view(void) {
    if (g_scroll_offset != 0) {
        g_scroll_offset = 0;
        g_selection_active = 0;
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

static void reset_text_attrs(void);

static void clear_screen(void) {
    reset_text_attrs();
    for (int y = 0; y < MAX_ROWS; y++) {
        for (int x = 0; x < MAX_COLS; x++) {
            clear_cell(&g_screen[y][x]);
        }
    }
    g_cx = 0;
    g_cy = 0;
    g_scroll_offset = 0;
}

static void clear_line_from_cursor(void) {
    for (int x = g_cx; x < g_cols; x++) {
        erase_cell(&g_screen[g_cy][x]);
    }
}

static void clear_line_to_cursor(void) {
    for (int x = 0; x <= g_cx && x < g_cols; x++) {
        erase_cell(&g_screen[g_cy][x]);
    }
}

static void clear_entire_line(void) {
    for (int x = 0; x < g_cols; x++) {
        erase_cell(&g_screen[g_cy][x]);
    }
}

static void erase_chars_from_cursor(int count) {
    int end = g_cx + count;
    if (end > g_cols) end = g_cols;
    for (int x = g_cx; x < end; x++) {
        erase_cell(&g_screen[g_cy][x]);
    }
}

static void clear_screen_from_cursor(void) {
    clear_line_from_cursor();
    for (int y = g_cy + 1; y < g_rows; y++) {
        for (int x = 0; x < g_cols; x++) {
            erase_cell(&g_screen[y][x]);
        }
    }
}

static void clear_screen_to_cursor(void) {
    for (int y = 0; y < g_cy; y++) {
        for (int x = 0; x < g_cols; x++) {
            erase_cell(&g_screen[y][x]);
        }
    }
    clear_line_to_cursor();
}

static void scroll_up(void) {
    push_history_line(g_screen[0]);
    for (int y = 1; y < g_rows; y++) {
        memcpy(g_screen[y - 1], g_screen[y], sizeof(Cell) * MAX_COLS);
    }
    clear_line(g_screen[g_rows - 1]);
    if (g_cy > 0) g_cy--;
}

static void newline(void) {
    g_cx = 0;
    g_cy++;
    if (g_cy >= g_rows) {
        scroll_up();
        g_cy = g_rows - 1;
    }
}

static void put_wchar(WCHAR ch) {
    if (ch == L'\r') {
        g_cx = 0;
        return;
    }
    if (ch == L'\n') {
        newline();
        return;
    }
    if (ch == L'\b') {
        if (g_cx > 0) g_cx--;
        return;
    }
    if (ch < 32) return;

    if (g_cx >= g_cols) newline();
    g_screen[g_cy][g_cx].ch = ch;
    g_screen[g_cy][g_cx].fg = g_cur_fg;
    g_screen[g_cy][g_cx].bg = g_cur_bg;
    g_cx++;
    if (g_cx >= g_cols) newline();
}

static int parse_num(const char* s, int* i, int fallback) {
    int n = 0;
    int seen = 0;
    while (s[*i] >= '0' && s[*i] <= '9') {
        n = n * 10 + (s[*i] - '0');
        (*i)++;
        seen = 1;
    }
    return seen ? n : fallback;
}

static COLORREF ansi_color(int index, int bright) {
    static const COLORREF normal[8] = {
        RGB(118, 118, 118),
        RGB(197, 15, 31),
        RGB(19, 161, 14),
        RGB(193, 156, 0),
        RGB(0, 55, 218),
        RGB(136, 23, 152),
        RGB(58, 150, 221),
        RGB(204, 204, 204)
    };
    static const COLORREF high[8] = {
        RGB(150, 150, 150),
        RGB(231, 72, 86),
        RGB(22, 198, 12),
        RGB(249, 241, 165),
        RGB(59, 120, 255),
        RGB(180, 0, 158),
        RGB(97, 214, 214),
        RGB(242, 242, 242)
    };

    if (index < 0 || index > 7) return DEFAULT_FG;
    return bright ? high[index] : normal[index];
}

static void refresh_text_attrs(void) {
    COLORREF fg;
    COLORREF bg;

    if (g_cur_fg_index >= 0) {
        fg = ansi_color(g_cur_fg_index, g_cur_fg_bright || g_bold);
    } else {
        fg = DEFAULT_FG;
    }

    if (g_cur_bg_index >= 0) {
        bg = ansi_color(g_cur_bg_index, g_cur_bg_bright);
    } else {
        bg = DEFAULT_BG;
    }

    if (g_reverse) {
        g_cur_fg = bg;
        g_cur_bg = fg;
    } else {
        g_cur_fg = fg;
        g_cur_bg = bg;
    }
}

static void reset_text_attrs(void) {
    g_cur_fg_index = -1;
    g_cur_bg_index = -1;
    g_cur_fg_bright = 0;
    g_cur_bg_bright = 0;
    g_bold = 0;
    g_reverse = 0;
    refresh_text_attrs();
}

static void handle_sgr_param(int p) {
    if (p == 0) {
        reset_text_attrs();
    } else if (p == 1) {
        g_bold = 1;
        refresh_text_attrs();
    } else if (p == 22) {
        g_bold = 0;
        refresh_text_attrs();
    } else if (p == 7) {
        g_reverse = 1;
        refresh_text_attrs();
    } else if (p == 27) {
        g_reverse = 0;
        refresh_text_attrs();
    } else if (p == 39) {
        g_cur_fg_index = -1;
        g_cur_fg_bright = 0;
        refresh_text_attrs();
    } else if (p == 49) {
        g_cur_bg_index = -1;
        g_cur_bg_bright = 0;
        refresh_text_attrs();
    } else if (p >= 30 && p <= 37) {
        g_cur_fg_index = p - 30;
        g_cur_fg_bright = 0;
        refresh_text_attrs();
    } else if (p >= 40 && p <= 47) {
        g_cur_bg_index = p - 40;
        g_cur_bg_bright = 0;
        refresh_text_attrs();
    } else if (p >= 90 && p <= 97) {
        g_cur_fg_index = p - 90;
        g_cur_fg_bright = 1;
        refresh_text_attrs();
    } else if (p >= 100 && p <= 107) {
        g_cur_bg_index = p - 100;
        g_cur_bg_bright = 1;
        refresh_text_attrs();
    }
}

static void handle_sgr(const char* seq, int len) {
    int end = len > 0 ? len - 1 : 0;
    int i = 0;
    int saw_param = 0;

    while (i < end) {
        int p;
        if (seq[i] == ';') {
            p = 0;
        } else {
            p = parse_num(seq, &i, 0);
        }

        if (p == 38 || p == 48) {
            int mode;
            if (i < end && seq[i] == ';') i++;
            mode = parse_num(seq, &i, 0);
            if (mode == 5) {
                if (i < end && seq[i] == ';') i++;
                (void)parse_num(seq, &i, 0);
            } else if (mode == 2) {
                for (int n = 0; n < 3; n++) {
                    if (i < end && seq[i] == ';') i++;
                    (void)parse_num(seq, &i, 0);
                }
            }
        } else {
            handle_sgr_param(p);
        }

        saw_param = 1;
        if (i < end && seq[i] == ';') i++;
    }

    if (!saw_param) {
        reset_text_attrs();
    }
}

static void handle_csi(const char* seq, int len) {
    char cmd = len > 0 ? seq[len - 1] : 0;
    int private_seq = len > 0 && seq[0] == '?';
    int i = private_seq ? 1 : 0;
    int default_a = (cmd == 'J' || cmd == 'K') ? 0 : 1;
    int a = parse_num(seq, &i, default_a);
    int b = 1;

    if (cmd == 'm') {
        handle_sgr(seq, len);
        return;
    }

    if (i < len && seq[i] == ';') {
        i++;
        b = parse_num(seq, &i, 1);
    }

    if (private_seq) {
        if (a == 25 && cmd == 'h') g_cursor_visible = 1;
        if (a == 25 && cmd == 'l') g_cursor_visible = 0;
        return;
    }

    switch (cmd) {
    case 'A':
        g_cy -= a;
        if (g_cy < 0) g_cy = 0;
        break;
    case 'B':
        g_cy += a;
        if (g_cy >= g_rows) g_cy = g_rows - 1;
        break;
    case 'C':
        g_cx += a;
        if (g_cx >= g_cols) g_cx = g_cols - 1;
        break;
    case 'D':
        g_cx -= a;
        if (g_cx < 0) g_cx = 0;
        break;
    case 'H':
    case 'f':
        g_cy = a - 1;
        g_cx = b - 1;
        if (g_cy < 0) g_cy = 0;
        if (g_cy >= g_rows) g_cy = g_rows - 1;
        if (g_cx < 0) g_cx = 0;
        if (g_cx >= g_cols) g_cx = g_cols - 1;
        break;
    case 'J':
        if (a == 0) clear_screen_from_cursor();
        if (a == 1) clear_screen_to_cursor();
        if (a == 2) clear_screen();
        if (a == 3) clear_scrollback();
        if (a == 3) clear_screen();
        break;
    case 'K':
        if (a == 0) clear_line_from_cursor();
        if (a == 1) clear_line_to_cursor();
        if (a == 2) clear_entire_line();
        break;
    case 'X':
        erase_chars_from_cursor(a);
        break;
    case 's':
        g_saved_cx = g_cx;
        g_saved_cy = g_cy;
        break;
    case 'u':
        g_cx = g_saved_cx;
        g_cy = g_saved_cy;
        break;
    default:
        break;
    }
}

static void append_osc_char(WCHAR ch) {
    if (g_osc_len < MAX_OSC - 1) {
        g_osc_buf[g_osc_len++] = ch;
    }
}

static void handle_osc(void) {
    int i = 0;
    int code = 0;
    WCHAR title[MAX_OSC + 16];

    g_osc_buf[g_osc_len] = 0;
    while (i < g_osc_len && g_osc_buf[i] >= L'0' && g_osc_buf[i] <= L'9') {
        code = code * 10 + (g_osc_buf[i] - L'0');
        i++;
    }
    if (i >= g_osc_len || g_osc_buf[i] != L';') return;
    i++;

    if ((code == 0 || code == 2) && g_osc_buf[i]) {
        lstrcpynW(title, APP_NAME L" - ", MAX_OSC);
        lstrcpynW(title + lstrlenW(title), g_osc_buf + i,
                  MAX_OSC - lstrlenW(title));
        SetWindowTextW(g_hwnd, title);
    }
}

static int utf8_sequence_len(unsigned char c) {
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) return 2;
    if (c >= 0xe0 && c <= 0xef) return 3;
    if (c >= 0xf0 && c <= 0xf4) return 4;
    return 0;
}

static int trailing_incomplete_utf8_len(const char* bytes, int len) {
    int start;
    int expected;
    int actual;

    if (len <= 0) return 0;

    start = len - 1;
    while (start > 0 &&
           (((unsigned char)bytes[start] & 0xc0) == 0x80) &&
           (len - start) < 4) {
        start--;
    }

    expected = utf8_sequence_len((unsigned char)bytes[start]);
    if (expected == 0) return 0;

    actual = len - start;
    return actual < expected ? actual : 0;
}

static int decode_output_bytes(const char* bytes, DWORD count, WCHAR* wbuf, int cap) {
    int total = g_utf8_pending_len + (int)count;
    int pending_len;
    int decode_len;
    int wlen;
    char* joined;

    if (total <= 0) return 0;
    joined = (char*)malloc((size_t)total);
    if (!joined) return 0;

    memcpy(joined, g_utf8_pending, (size_t)g_utf8_pending_len);
    memcpy(joined + g_utf8_pending_len, bytes, (size_t)count);
    g_utf8_pending_len = 0;

    pending_len = trailing_incomplete_utf8_len(joined, total);
    decode_len = total - pending_len;
    if (decode_len <= 0) {
        memcpy(g_utf8_pending, joined, (size_t)pending_len);
        g_utf8_pending_len = pending_len;
        free(joined);
        return 0;
    }

    wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               joined, decode_len, wbuf, cap);
    if (wlen > 0 && pending_len > 0) {
        memcpy(g_utf8_pending, joined + decode_len, (size_t)pending_len);
        g_utf8_pending_len = pending_len;
    }
    if (wlen <= 0) {
        g_utf8_pending_len = 0;
        wlen = MultiByteToWideChar(CP_ACP, 0, joined, total, wbuf, cap);
    }
    free(joined);
    return wlen;
}

static void feed_output(const char* bytes, DWORD count) {
    WCHAR wbuf[8192];
    int wlen = decode_output_bytes(bytes, count, wbuf, 8192);
    if (wlen <= 0) return;

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < wlen; i++) {
        WCHAR ch = wbuf[i];

        switch (g_escape_state) {
        case ESC_NORMAL:
            if (ch == 0x1b) {
                g_escape_state = ESC_SEEN;
            } else {
                put_wchar(ch);
            }
            break;

        case ESC_SEEN:
            if (ch == L'[') {
                g_escape_state = ESC_CSI;
                g_csi_len = 0;
            } else if (ch == L']') {
                g_escape_state = ESC_OSC;
                g_osc_len = 0;
            } else if (ch == L'c') {
                reset_text_attrs();
                clear_screen();
                clear_scrollback();
                g_escape_state = ESC_NORMAL;
            } else {
                g_escape_state = ESC_NORMAL;
            }
            break;

        case ESC_CSI:
            if (g_csi_len < (int)sizeof(g_csi_buf) - 1) {
                g_csi_buf[g_csi_len++] = (char)ch;
            }
            if (ch >= 0x40 && ch <= 0x7e) {
                g_csi_buf[g_csi_len] = 0;
                handle_csi(g_csi_buf, g_csi_len);
                g_escape_state = ESC_NORMAL;
                g_csi_len = 0;
            }
            break;

        case ESC_OSC:
            if (ch == 0x07) {
                handle_osc();
                g_escape_state = ESC_NORMAL;
            } else if (ch == 0x1b) {
                g_escape_state = ESC_OSC_ESC;
            } else {
                append_osc_char(ch);
            }
            break;

        case ESC_OSC_ESC:
            if (ch == L'\\') {
                handle_osc();
                g_escape_state = ESC_NORMAL;
            } else if (ch == 0x1b) {
                g_escape_state = ESC_OSC_ESC;
            } else {
                append_osc_char(0x1b);
                append_osc_char(ch);
                g_escape_state = ESC_OSC;
            }
            break;
        }
    }
    LeaveCriticalSection(&g_lock);
    schedule_terminal_repaint();
}

static void mouse_to_cell(LPARAM lp, int* cell_x, int* cell_y) {
    int px = (int)(short)LOWORD(lp);
    int py = (int)(short)HIWORD(lp);
    int x = (px - 8) / g_char_w;
    int y = (py - 8) / g_char_h;

    if (px < 8) x = 0;
    if (py < 8) y = 0;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= g_cols) x = g_cols - 1;
    if (y >= g_rows) y = g_rows - 1;

    *cell_x = x;
    *cell_y = y;
}

static void selection_bounds(int* sx, int* sy, int* ex, int* ey) {
    if (g_sel_anchor_y < g_sel_focus_y ||
        (g_sel_anchor_y == g_sel_focus_y && g_sel_anchor_x <= g_sel_focus_x)) {
        *sx = g_sel_anchor_x;
        *sy = g_sel_anchor_y;
        *ex = g_sel_focus_x;
        *ey = g_sel_focus_y;
    } else {
        *sx = g_sel_focus_x;
        *sy = g_sel_focus_y;
        *ex = g_sel_anchor_x;
        *ey = g_sel_anchor_y;
    }
}

static int selection_has_text(void) {
    return g_selection_active &&
           (g_sel_anchor_x != g_sel_focus_x || g_sel_anchor_y != g_sel_focus_y);
}

static int cell_is_selected(int x, int y) {
    int sx, sy, ex, ey;
    if (!selection_has_text()) return 0;
    selection_bounds(&sx, &sy, &ex, &ey);
    if (y < sy || y > ey) return 0;
    if (y == sy && x < sx) return 0;
    if (y == ey && x > ex) return 0;
    return 1;
}

static int word_char(WCHAR ch) {
    return ch > L' ' && ch != 0;
}

static int word_bounds_at(int x, int y, int* start, int* end) {
    const Cell* line;

    if (x < 0 || x >= g_cols || y < 0 || y >= g_rows) return 0;

    EnterCriticalSection(&g_lock);
    line = view_line_at(y);
    if (!word_char(line[x].ch)) {
        LeaveCriticalSection(&g_lock);
        return 0;
    }

    *start = x;
    *end = x;
    while (*start > 0 && word_char(line[*start - 1].ch)) (*start)--;
    while (*end + 1 < g_cols && word_char(line[*end + 1].ch)) (*end)++;
    LeaveCriticalSection(&g_lock);
    return 1;
}

static void set_word_selection_range(int focus_start, int focus_end, int focus_y) {
    if (focus_y < g_word_anchor_y ||
        (focus_y == g_word_anchor_y && focus_end < g_word_anchor_start_x)) {
        g_sel_anchor_x = g_word_anchor_end_x;
        g_sel_anchor_y = g_word_anchor_y;
        g_sel_focus_x = focus_start;
        g_sel_focus_y = focus_y;
    } else {
        g_sel_anchor_x = g_word_anchor_start_x;
        g_sel_anchor_y = g_word_anchor_y;
        g_sel_focus_x = focus_end;
        g_sel_focus_y = focus_y;
    }
    g_selection_active = 1;
}

static int select_word_at(int x, int y) {
    int start;
    int end;

    if (!word_bounds_at(x, y, &start, &end)) return 0;

    g_sel_anchor_x = start;
    g_sel_anchor_y = y;
    g_sel_focus_x = end;
    g_sel_focus_y = y;
    g_selection_active = 1;
    g_selecting = 0;
    g_word_selecting = 0;
    copy_selection_to_clipboard();
    InvalidateRect(g_hwnd, NULL, FALSE);
    return 1;
}

static int start_word_selection_at(int x, int y) {
    int start;
    int end;

    if (!word_bounds_at(x, y, &start, &end)) return 0;
    g_word_anchor_start_x = start;
    g_word_anchor_end_x = end;
    g_word_anchor_y = y;
    set_word_selection_range(start, end, y);
    g_selecting = 1;
    g_word_selecting = 1;
    SetCapture(g_hwnd);
    InvalidateRect(g_hwnd, NULL, FALSE);
    return 1;
}

static void update_word_selection_at(int x, int y) {
    int start;
    int end;

    if (!word_bounds_at(x, y, &start, &end)) {
        start = x;
        end = x;
    }
    set_word_selection_range(start, end, y);
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static int copy_selection_to_clipboard(void) {
    int sx, sy, ex, ey;
    int capacity;
    int pos = 0;
    WCHAR* text;
    HGLOBAL mem;
    WCHAR* dst;

    if (!selection_has_text()) return 0;
    selection_bounds(&sx, &sy, &ex, &ey);
    capacity = (ey - sy + 1) * (MAX_COLS + 2) + 1;
    text = (WCHAR*)malloc(sizeof(WCHAR) * capacity);
    if (!text) return 0;

    EnterCriticalSection(&g_lock);
    for (int y = sy; y <= ey; y++) {
        const Cell* line = view_line_at(y);
        int row_start = (y == sy) ? sx : 0;
        int row_end = (y == ey) ? ex : g_cols - 1;
        while (row_end >= row_start && line[row_end].ch == L' ') {
            row_end--;
        }
        for (int x = row_start; x <= row_end; x++) {
            text[pos++] = line[x].ch;
        }
        if (y != ey) {
            text[pos++] = L'\r';
            text[pos++] = L'\n';
        }
    }
    LeaveCriticalSection(&g_lock);

    text[pos] = 0;
    mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(WCHAR) * (pos + 1));
    if (!mem) {
        free(text);
        return 0;
    }
    dst = (WCHAR*)GlobalLock(mem);
    if (!dst) {
        GlobalFree(mem);
        free(text);
        return 0;
    }
    memcpy(dst, text, sizeof(WCHAR) * (pos + 1));
    GlobalUnlock(mem);
    free(text);

    if (!OpenClipboard(g_hwnd)) {
        GlobalFree(mem);
        return 0;
    }
    EmptyClipboard();
    SetClipboardData(CF_UNICODETEXT, mem);
    CloseClipboard();
    return 1;
}

static int paste_clipboard_text(void) {
    HANDLE clip;
    WCHAR* src;
    WCHAR* normalized;
    char* utf8;
    int src_len;
    int norm_len = 0;
    int utf8_len;

    if (!OpenClipboard(g_hwnd)) return 0;
    clip = GetClipboardData(CF_UNICODETEXT);
    if (!clip) {
        CloseClipboard();
        return 0;
    }

    src = (WCHAR*)GlobalLock(clip);
    if (!src) {
        CloseClipboard();
        return 0;
    }

    src_len = lstrlenW(src);
    normalized = (WCHAR*)malloc(sizeof(WCHAR) * (src_len + 1));
    if (!normalized) {
        GlobalUnlock(clip);
        CloseClipboard();
        return 0;
    }

    for (int i = 0; i < src_len; i++) {
        if (src[i] == L'\r') {
            normalized[norm_len++] = L'\r';
            if (i + 1 < src_len && src[i + 1] == L'\n') i++;
        } else if (src[i] == L'\n') {
            normalized[norm_len++] = L'\r';
        } else {
            normalized[norm_len++] = src[i];
        }
    }
    normalized[norm_len] = 0;
    GlobalUnlock(clip);
    CloseClipboard();

    utf8_len = WideCharToMultiByte(CP_UTF8, 0, normalized, norm_len, NULL, 0, NULL, NULL);
    if (utf8_len <= 0) {
        free(normalized);
        return 0;
    }

    utf8 = (char*)malloc((size_t)utf8_len);
    if (!utf8) {
        free(normalized);
        return 0;
    }
    WideCharToMultiByte(CP_UTF8, 0, normalized, norm_len, utf8, utf8_len, NULL, NULL);
    free(normalized);
    backend_write(utf8, (DWORD)utf8_len);
    free(utf8);
    return 1;
}

static void post_backend_output(const char* data, DWORD len, void* user) {
    HWND hwnd = (HWND)user;
    char* copy;

    if (!data || len == 0) {
        PostMessageW(hwnd, WM_BACKEND_EXIT, 0, 0);
        return;
    }

    log_backend_output(data, len);
    copy = (char*)malloc(len);
    if (!copy) return;
    memcpy(copy, data, len);
    if (!PostMessageW(hwnd, WM_BACKEND_DATA, (WPARAM)len, (LPARAM)copy)) {
        free(copy);
    }
}

static int backend_write(const char* s, DWORD len) {
    if (!g_backend.write) return 0;
    return_to_live_view();
    return g_backend.write(&g_backend, s, len);
}

static void update_metrics(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    HFONT old = (HFONT)SelectObject(dc, g_font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    g_char_w = tm.tmAveCharWidth;
    g_char_h = tm.tmHeight + tm.tmExternalLeading;
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
}

static void resize_grid(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    int cols = (rc.right - rc.left - 16) / g_char_w;
    int rows = (rc.bottom - rc.top - 16) / g_char_h;
    if (cols < 20) cols = 20;
    if (rows < 8) rows = 8;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    if (cols == g_cols && rows == g_rows) return;

    EnterCriticalSection(&g_lock);
    g_cols = cols;
    g_rows = rows;
    if (g_cx >= g_cols) g_cx = g_cols - 1;
    if (g_cy >= g_rows) g_cy = g_rows - 1;
    LeaveCriticalSection(&g_lock);

    if (g_backend.resize) {
        g_backend.resize(&g_backend, g_cols, g_rows);
    }
}

static void paint_terminal(HWND hwnd, HDC dc) {
    RECT rc;
    HBRUSH bg;
    HFONT old_font;

    GetClientRect(hwnd, &rc);
    bg = CreateSolidBrush(RGB(12, 16, 20));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    old_font = (HFONT)SelectObject(dc, g_font);
    SetBkMode(dc, OPAQUE);

    EnterCriticalSection(&g_lock);
    for (int y = 0; y < g_rows; y++) {
        const Cell* line = view_line_at(y);
        for (int x = 0; x < g_cols; x++) {
            WCHAR ch = line[x].ch;
            COLORREF fg = line[x].fg;
            COLORREF bgc = line[x].bg;
            if (cell_is_selected(x, y)) {
                fg = RGB(12, 16, 20);
                bgc = RGB(140, 190, 255);
            }
            SetTextColor(dc, fg);
            SetBkColor(dc, bgc);
            TextOutW(dc, 8 + x * g_char_w, 8 + y * g_char_h, &ch, 1);
        }
    }

    if (g_scroll_offset == 0 && g_cursor_visible) {
        RECT cursor = {
            8 + g_cx * g_char_w,
            8 + g_cy * g_char_h,
            8 + (g_cx + 1) * g_char_w,
            8 + (g_cy + 1) * g_char_h
        };
        InvertRect(dc, &cursor);
    }
    LeaveCriticalSection(&g_lock);

    SelectObject(dc, old_font);
}

static void paint_terminal_buffered(HWND hwnd, HDC target_dc) {
    RECT rc;
    int width;
    int height;
    HDC mem_dc;
    HBITMAP bitmap;
    HBITMAP old_bitmap;

    GetClientRect(hwnd, &rc);
    width = rc.right - rc.left;
    height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    mem_dc = CreateCompatibleDC(target_dc);
    if (!mem_dc) {
        paint_terminal(hwnd, target_dc);
        return;
    }

    bitmap = CreateCompatibleBitmap(target_dc, width, height);
    if (!bitmap) {
        DeleteDC(mem_dc);
        paint_terminal(hwnd, target_dc);
        return;
    }

    old_bitmap = (HBITMAP)SelectObject(mem_dc, bitmap);
    paint_terminal(hwnd, mem_dc);
    BitBlt(target_dc, 0, 0, width, height, mem_dc, 0, 0, SRCCOPY);
    SelectObject(mem_dc, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(mem_dc);
}

static int send_ctrl_key(WPARAM vk) {
    char c;
    if (vk >= 'A' && vk <= 'Z') {
        c = (char)(vk - 'A' + 1);
        backend_write(&c, 1);
        return 1;
    }
    return 0;
}

static void send_virtual_key(WPARAM vk) {
    switch (vk) {
    case VK_UP: backend_write("\x1b[A", 3); break;
    case VK_DOWN: backend_write("\x1b[B", 3); break;
    case VK_RIGHT: backend_write("\x1b[C", 3); break;
    case VK_LEFT: backend_write("\x1b[D", 3); break;
    case VK_HOME: backend_write("\x1b[H", 3); break;
    case VK_END: backend_write("\x1b[F", 3); break;
    case VK_DELETE: backend_write("\x1b[3~", 4); break;
    case VK_TAB: backend_write("\t", 1); break;
    case VK_BACK: backend_write("\x7f", 1); break;
    default: break;
    }
}

static int send_ctrl_navigation_key(WPARAM vk) {
    switch (vk) {
    case VK_RIGHT: backend_write("\x1b[1;5C", 6); return 1;
    case VK_LEFT: backend_write("\x1b[1;5D", 6); return 1;
    default: return 0;
    }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        if (!g_font) {
            g_font = (HFONT)GetStockObject(ANSI_FIXED_FONT);
        }
        update_metrics(hwnd);
        resize_grid(hwnd);
        clear_screen();
        open_output_log();
        if (!term_backend_conpty_init(&g_backend)) {
            die_box(L"Could not initialize the local ConPTY backend.");
            return -1;
        }
        if (!g_backend.start(&g_backend, g_cols, g_rows, post_backend_output, hwnd)) {
            die_box(L"Could not start cmd.exe through ConPTY. This prototype needs Windows 10 1809 or newer.");
            return -1;
        }
        SetFocus(hwnd);
        return 0;

    case WM_SIZE:
        resize_grid(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_CHAR:
        if (wp == L'\r') {
            backend_write("\r", 1);
        } else if (wp >= 32) {
            WCHAR wc = (WCHAR)wp;
            char utf8[8];
            int len = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, utf8, sizeof(utf8), NULL, NULL);
            if (len > 0) backend_write(utf8, (DWORD)len);
        }
        return 0;

    case WM_KEYDOWN:
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000)) {
            if (wp == 'C') {
                copy_selection_to_clipboard();
                return 0;
            }
            if (wp == 'V') {
                paste_clipboard_text();
                return 0;
            }
            if (wp == 'W') {
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                return 0;
            }
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == VK_HOME) {
            set_scroll_offset(g_history_count);
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == VK_END) {
            set_scroll_offset(0);
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && send_ctrl_navigation_key(wp)) {
            return 0;
        }
        if (wp == VK_PRIOR) {
            scroll_view(g_rows - 1);
            return 0;
        }
        if (wp == VK_NEXT) {
            scroll_view(-(g_rows - 1));
            return 0;
        }
        if (!(GetKeyState(VK_CONTROL) & 0x8000) || !send_ctrl_key(wp)) {
            send_virtual_key(wp);
        }
        return 0;

    case WM_MOUSEWHEEL:
        if ((SHORT)HIWORD(wp) > 0) {
            scroll_view(3);
        } else if ((SHORT)HIWORD(wp) < 0) {
            scroll_view(-3);
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_REPAINT) {
            KillTimer(hwnd, TIMER_REPAINT);
            g_repaint_pending = 0;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        break;

    case WM_PASTE:
        paste_clipboard_text();
        return 0;

    case WM_LBUTTONDOWN:
        mouse_to_cell(lp, &g_sel_anchor_x, &g_sel_anchor_y);
        g_sel_focus_x = g_sel_anchor_x;
        g_sel_focus_y = g_sel_anchor_y;
        g_selection_active = 1;
        g_selecting = 1;
        SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_MOUSEMOVE:
        if (g_selecting) {
            if (g_word_selecting) {
                int x;
                int y;
                mouse_to_cell(lp, &x, &y);
                update_word_selection_at(x, y);
            } else {
                mouse_to_cell(lp, &g_sel_focus_x, &g_sel_focus_y);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONUP:
        if (g_selecting) {
            if (g_word_selecting) {
                int x;
                int y;
                mouse_to_cell(lp, &x, &y);
                update_word_selection_at(x, y);
            } else {
                mouse_to_cell(lp, &g_sel_focus_x, &g_sel_focus_y);
            }
            g_selecting = 0;
            g_word_selecting = 0;
            ReleaseCapture();
            if (selection_has_text()) {
                copy_selection_to_clipboard();
            } else {
                g_selection_active = 0;
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_LBUTTONDBLCLK: {
        int x;
        int y;
        if (g_selecting) {
            g_selecting = 0;
            g_word_selecting = 0;
            ReleaseCapture();
        }
        mouse_to_cell(lp, &x, &y);
        start_word_selection_at(x, y);
        return 0;
    }

    case WM_RBUTTONDOWN:
        paste_clipboard_text();
        return 0;

    case WM_BACKEND_DATA:
        feed_output((const char*)lp, (DWORD)wp);
        free((void*)lp);
        return 0;

    case WM_BACKEND_EXIT:
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_terminal_buffered(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_SETFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REPAINT);
        if (g_backend.stop) g_backend.stop(&g_backend);
        close_output_log();
        if (g_font) DeleteObject(g_font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev;
    (void)cmdline;
    InitializeCriticalSection(&g_lock);

    WNDCLASSW wc = {0};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = L"TermuWindow";
    wc.hbrBackground = NULL;
    if (!RegisterClassW(&wc)) return 1;

    {
        RECT desired = {0, 0, 900, 560};
        HWND hwnd;
        AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, FALSE);
        hwnd = CreateWindowExW(0, wc.lpszClassName, APP_NAME,
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               CW_USEDEFAULT, CW_USEDEFAULT,
                               desired.right - desired.left,
                               desired.bottom - desired.top,
                               NULL, NULL, inst, NULL);
        if (!hwnd) return 1;

        ShowWindow(hwnd, show);
        UpdateWindow(hwnd);
    }

    {
        MSG msg;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        DeleteCriticalSection(&g_lock);
        return (int)msg.wParam;
    }
}
