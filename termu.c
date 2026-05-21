// Termu: tiny SecureCRT-style terminal prototype for Windows.
//
// Version: 0.2 - backend abstraction plus local Windows ConPTY backend.

#define _WIN32_WINNT 0x0A00
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4115)
#endif
#include <icmpapi.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "term_backend.h"

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

#define APP_NAME L"Termu v0.2"
#define WM_BACKEND_DATA (WM_APP + 1)
#define WM_BACKEND_EXIT (WM_APP + 2)
#define WM_SCAN_DONE (WM_APP + 3)
#define TIMER_REPAINT 1
#define MAX_ROWS 80
#define MAX_COLS 160
#define MAX_HISTORY 4000
#define MAX_OSC 512
#define START_ROWS 32
#define START_COLS 100
#define DEFAULT_FG RGB(230, 230, 230)
#define DEFAULT_BG RGB(12, 16, 20)
#define HOST_PANEL_W 140
#define TAB_BAR_H 26
#define TERM_PAD 6
#define HOST_ROW_H 22
#define TAB_W 126
#define MAX_SESSIONS 8
#define MAX_HOSTS 16
#define HOST_NAME_MAX 64
#define HOST_COMMAND_MAX 256
#define HOST_PASSWORD_MAX 256
#define HOST_PASSWORD_BLOB_MAX 2048
#define MAX_DISCOVERED 64
#define SCAN_WORKERS 16

typedef struct {
    int prefix[3];
    volatile LONG next_host;
    volatile LONG active_workers;
} ScanState;

typedef struct {
    WCHAR ch;
    COLORREF fg;
    COLORREF bg;
} Cell;

typedef struct {
    WCHAR name[HOST_NAME_MAX];
    char command[HOST_COMMAND_MAX];
    char password_blob[HOST_PASSWORD_BLOB_MAX];
} HostEntry;

typedef struct {
    WCHAR label[96];
    char ip[16];
} DiscoveredHost;

typedef enum {
    ESC_NORMAL,
    ESC_SEEN,
    ESC_CSI,
    ESC_OSC,
    ESC_OSC_ESC
} EscapeState;

typedef struct {
    TermBackend backend;
    WCHAR name[64];
    WCHAR title[MAX_OSC];
    int host_index;
    int exited;
    int cx;
    int cy;
    int saved_cx;
    int saved_cy;
    int cursor_visible;
    COLORREF cur_fg;
    COLORREF cur_bg;
    int cur_fg_index;
    int cur_bg_index;
    int cur_fg_bright;
    int cur_bg_bright;
    int bold;
    int reverse;
    Cell screen[MAX_ROWS][MAX_COLS];
    Cell history[MAX_HISTORY][MAX_COLS];
    int history_start;
    int history_count;
    int scroll_offset;
    char utf8_pending[4];
    int utf8_pending_len;
    EscapeState escape_state;
    char csi_buf[64];
    int csi_len;
    WCHAR osc_buf[MAX_OSC];
    int osc_len;
    char prompt_tail[64];
    int prompt_tail_len;
    int password_capture;
    char password_input[HOST_PASSWORD_MAX];
    int password_input_len;
} TermSession;

typedef struct {
    TermSession* session;
    DWORD len;
    char data[1];
} BackendData;

static HWND g_hwnd;
static HFONT g_font;
static HICON g_app_icon;
static int g_char_w = 8;
static int g_char_h = 16;
static int g_rows = START_ROWS;
static int g_cols = START_COLS;
static CRITICAL_SECTION g_lock;
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
static HostEntry g_hosts[MAX_HOSTS];
static int g_host_count = 0;
static DiscoveredHost g_discovered[MAX_DISCOVERED];
static int g_discovered_count = 0;
static volatile LONG g_scan_running = 0;
static CRITICAL_SECTION g_scan_lock;
static TermSession* g_sessions[MAX_SESSIONS];
static int g_session_count = 0;
static int g_active_session = -1;
static TermSession* g_session = NULL;

#define g_backend (g_session->backend)
#define g_cx (g_session->cx)
#define g_cy (g_session->cy)
#define g_saved_cx (g_session->saved_cx)
#define g_saved_cy (g_session->saved_cy)
#define g_cursor_visible (g_session->cursor_visible)
#define g_cur_fg (g_session->cur_fg)
#define g_cur_bg (g_session->cur_bg)
#define g_cur_fg_index (g_session->cur_fg_index)
#define g_cur_bg_index (g_session->cur_bg_index)
#define g_cur_fg_bright (g_session->cur_fg_bright)
#define g_cur_bg_bright (g_session->cur_bg_bright)
#define g_bold (g_session->bold)
#define g_reverse (g_session->reverse)
#define g_screen (g_session->screen)
#define g_history (g_session->history)
#define g_history_start (g_session->history_start)
#define g_history_count (g_session->history_count)
#define g_scroll_offset (g_session->scroll_offset)
#define g_utf8_pending (g_session->utf8_pending)
#define g_utf8_pending_len (g_session->utf8_pending_len)
#define g_escape_state (g_session->escape_state)
#define g_csi_buf (g_session->csi_buf)
#define g_csi_len (g_session->csi_len)
#define g_osc_buf (g_session->osc_buf)
#define g_osc_len (g_session->osc_len)

static void die_box(const WCHAR* text) {
    MessageBoxW(NULL, text, APP_NAME, MB_OK | MB_ICONERROR);
}

static int backend_write(const char* s, DWORD len);
static int copy_selection_to_clipboard(void);
static int start_host_session(int host_index);
static int start_command_session(const WCHAR* name, const char* command, int host_index);
static void start_lan_scan(void);
static void switch_session(int index);
static void close_session(int index);

static TermSession* active_session(void) {
    if (g_active_session < 0 || g_active_session >= g_session_count) return NULL;
    return g_sessions[g_active_session];
}

static void set_active_session(int index) {
    if (index < 0) {
        g_active_session = -1;
        g_session = NULL;
        return;
    }
    if (index >= g_session_count || !g_sessions[index]) return;
    g_active_session = index;
    g_session = g_sessions[index];
}

static void fill_icon_rect(DWORD* pixels, int x, int y, int w, int h, DWORD color) {
    for (int py = y; py < y + h; py++) {
        for (int px = x; px < x + w; px++) {
            if (px >= 0 && px < 32 && py >= 0 && py < 32) {
                pixels[py * 32 + px] = color;
            }
        }
    }
}

static void fill_solid_rect(HDC dc, const RECT* rc, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, rc, brush);
    DeleteObject(brush);
}

static HICON create_app_icon(void) {
    BITMAPV5HEADER bi = {0};
    ICONINFO ii = {0};
    HDC dc;
    HBITMAP color_bitmap;
    HBITMAP mask_bitmap;
    BYTE mask_bits[32 * 4] = {0};
    DWORD* pixels = NULL;
    HICON icon;

    bi.bV5Size = sizeof(bi);
    bi.bV5Width = 32;
    bi.bV5Height = -32;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00ff0000;
    bi.bV5GreenMask = 0x0000ff00;
    bi.bV5BlueMask = 0x000000ff;
    bi.bV5AlphaMask = 0xff000000;

    dc = GetDC(NULL);
    color_bitmap = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS,
                                    (void**)&pixels, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color_bitmap || !pixels) {
        if (color_bitmap) DeleteObject(color_bitmap);
        return NULL;
    }

    fill_icon_rect(pixels, 0, 0, 32, 32, 0xff07111fu);
    fill_icon_rect(pixels, 4, 3, 24, 7, 0xff55b7ffu);
    fill_icon_rect(pixels, 12, 10, 8, 18, 0xff55b7ffu);

    mask_bitmap = CreateBitmap(32, 32, 1, 1, mask_bits);
    if (!mask_bitmap) {
        DeleteObject(color_bitmap);
        return NULL;
    }

    ii.fIcon = TRUE;
    ii.hbmColor = color_bitmap;
    ii.hbmMask = mask_bitmap;
    icon = CreateIconIndirect(&ii);

    DeleteObject(mask_bitmap);
    DeleteObject(color_bitmap);
    return icon;
}

static void strip_line_end(char* s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = 0;
    }
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_encode(const BYTE* data, DWORD len, char* out, int out_cap) {
    static const char digits[] = "0123456789abcdef";
    if ((DWORD)out_cap <= len * 2) return 0;
    for (DWORD i = 0; i < len; i++) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }
    out[len * 2] = 0;
    return 1;
}

static int hex_decode(const char* hex, BYTE* out, DWORD out_cap, DWORD* out_len) {
    DWORD len = (DWORD)strlen(hex);
    if ((len % 2) != 0 || len / 2 > out_cap) return 0;
    for (DWORD i = 0; i < len / 2; i++) {
        int hi = hex_value(hex[i * 2]);
        int lo = hex_value(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[i] = (BYTE)((hi << 4) | lo);
    }
    *out_len = len / 2;
    return 1;
}

static int protect_password(const char* password, char* out, int out_cap) {
    DATA_BLOB in;
    DATA_BLOB protected_blob;
    int ok;

    if (!password || !password[0] || out_cap <= 6) return 0;
    in.pbData = (BYTE*)password;
    in.cbData = (DWORD)strlen(password);
    if (!CryptProtectData(&in, L"Termu host password", NULL, NULL, NULL, 0,
                          &protected_blob)) {
        return 0;
    }
    lstrcpynA(out, "dpapi:", out_cap);
    ok = hex_encode(protected_blob.pbData, protected_blob.cbData,
                    out + 6, out_cap - 6);
    LocalFree(protected_blob.pbData);
    if (!ok) out[0] = 0;
    return ok;
}

static int unprotect_password(const char* blob, char* password, int password_cap) {
    BYTE encrypted[HOST_PASSWORD_BLOB_MAX / 2];
    DWORD encrypted_len = 0;
    DATA_BLOB in;
    DATA_BLOB plain;
    DWORD copy_len;

    if (!blob || strncmp(blob, "dpapi:", 6) != 0) return 0;
    if (!hex_decode(blob + 6, encrypted, sizeof(encrypted), &encrypted_len)) return 0;

    in.pbData = encrypted;
    in.cbData = encrypted_len;
    if (!CryptUnprotectData(&in, NULL, NULL, NULL, NULL, 0, &plain)) return 0;

    copy_len = plain.cbData;
    if (copy_len >= (DWORD)password_cap) copy_len = (DWORD)password_cap - 1;
    memcpy(password, plain.pbData, copy_len);
    password[copy_len] = 0;
    SecureZeroMemory(plain.pbData, plain.cbData);
    LocalFree(plain.pbData);
    return 1;
}

static HostEntry* add_host_entry(const char* name, const char* command,
                                 const char* password_blob) {
    HostEntry* host;
    int wlen;

    if (!name || !name[0] || g_host_count >= MAX_HOSTS) return NULL;
    host = &g_hosts[g_host_count++];
    ZeroMemory(host, sizeof(*host));

    wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                               name, -1, host->name, HOST_NAME_MAX);
    if (wlen <= 0) {
        MultiByteToWideChar(CP_ACP, 0, name, -1, host->name, HOST_NAME_MAX);
    }
    host->name[HOST_NAME_MAX - 1] = 0;
    if (command) {
        lstrcpynA(host->command, command, HOST_COMMAND_MAX);
        host->command[HOST_COMMAND_MAX - 1] = 0;
    }
    if (password_blob) {
        lstrcpynA(host->password_blob, password_blob, HOST_PASSWORD_BLOB_MAX);
        host->password_blob[HOST_PASSWORD_BLOB_MAX - 1] = 0;
    }
    return host;
}

static void add_default_hosts(void) {
    g_host_count = 0;
    add_host_entry("Local cmd", "", "");
    add_host_entry("nybo-linux", "ssh nybo@nybo-loq-15irx10.lan", "");
}

static void save_hosts(void) {
    HANDLE file = CreateFileW(L"termu_hosts.txt", GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;

    for (int i = 0; i < g_host_count; i++) {
        char name[HOST_NAME_MAX * 4];
        char line[HOST_NAME_MAX * 4 + HOST_COMMAND_MAX + HOST_PASSWORD_BLOB_MAX + 8];
        DWORD written = 0;
        int name_len = WideCharToMultiByte(CP_UTF8, 0, g_hosts[i].name, -1,
                                           name, sizeof(name), NULL, NULL);
        if (name_len <= 0) continue;
        if (g_hosts[i].password_blob[0]) {
            snprintf(line, sizeof(line), "%s|%s|%s\r\n",
                     name, g_hosts[i].command, g_hosts[i].password_blob);
        } else {
            snprintf(line, sizeof(line), "%s|%s\r\n", name, g_hosts[i].command);
        }
        WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
    }
    CloseHandle(file);
}

static void load_hosts(void) {
    HANDLE file;
    DWORD size;
    DWORD read = 0;
    char* data;
    char* line;
    char* next;

    g_host_count = 0;
    file = CreateFileW(L"termu_hosts.txt", GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        add_default_hosts();
        return;
    }

    size = GetFileSize(file, NULL);
    if (size == INVALID_FILE_SIZE || size > 65536) {
        CloseHandle(file);
        add_default_hosts();
        return;
    }

    data = (char*)malloc((size_t)size + 1);
    if (!data) {
        CloseHandle(file);
        add_default_hosts();
        return;
    }

    if (!ReadFile(file, data, size, &read, NULL)) read = 0;
    CloseHandle(file);
    data[read] = 0;

    line = data;
    while (line && *line) {
        char* sep;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        strip_line_end(line);
        if (line[0] && line[0] != '#') {
            sep = strchr(line, '|');
            if (sep) {
                char* password_blob;
                *sep++ = 0;
                password_blob = strchr(sep, '|');
                if (password_blob) *password_blob++ = 0;
                strip_line_end(sep);
                if (password_blob) strip_line_end(password_blob);
                add_host_entry(line, sep, password_blob ? password_blob : "");
            } else {
                add_host_entry(line, "", "");
            }
        }
        line = next;
    }
    free(data);

    if (g_host_count == 0) add_default_hosts();
}

static int parse_ipv4(const char* s, int octets[4]) {
    int n = sscanf(s, "%d.%d.%d.%d", &octets[0], &octets[1], &octets[2], &octets[3]);
    if (n != 4) return 0;
    for (int i = 0; i < 4; i++) {
        if (octets[i] < 0 || octets[i] > 255) return 0;
    }
    return 1;
}

static int find_lan_prefix(int prefix[3]) {
    ULONG size = 0;
    IP_ADAPTER_INFO* adapters;
    DWORD rc = GetAdaptersInfo(NULL, &size);
    if (rc != ERROR_BUFFER_OVERFLOW || size == 0) return 0;

    adapters = (IP_ADAPTER_INFO*)malloc(size);
    if (!adapters) return 0;
    rc = GetAdaptersInfo(adapters, &size);
    if (rc == ERROR_SUCCESS) {
        for (IP_ADAPTER_INFO* adapter = adapters; adapter; adapter = adapter->Next) {
            IP_ADDR_STRING* addr = &adapter->IpAddressList;
            while (addr) {
                int ip[4];
                int mask[4];
                if (parse_ipv4(addr->IpAddress.String, ip) &&
                    parse_ipv4(addr->IpMask.String, mask) &&
                    ip[0] != 0 && ip[0] != 127 &&
                    mask[0] == 255 && mask[1] == 255 && mask[2] == 255 && mask[3] == 0) {
                    prefix[0] = ip[0];
                    prefix[1] = ip[1];
                    prefix[2] = ip[2];
                    free(adapters);
                    return 1;
                }
                addr = addr->Next;
            }
        }
    }
    free(adapters);
    return 0;
}

static DWORD ipaddr_from_octets(int a, int b, int c, int d) {
    return (DWORD)(a | (b << 8) | (c << 16) | (d << 24));
}

static void resolve_discovered_label(const char* ip, WCHAR* label, int label_cap) {
    DWORD addr = inet_addr(ip);
    struct hostent* host = NULL;
    if (addr != INADDR_NONE) {
        host = gethostbyaddr((const char*)&addr, sizeof(addr), AF_INET);
    }
    if (host && host->h_name && host->h_name[0]) {
        MultiByteToWideChar(CP_ACP, 0, host->h_name, -1, label, label_cap);
        label[label_cap - 1] = 0;
    } else {
        MultiByteToWideChar(CP_ACP, 0, ip, -1, label, label_cap);
    }
}

static void add_discovered_ip(int a, int b, int c, int d) {
    DiscoveredHost* host;
    char ip[16];
    WCHAR label[96];

    snprintf(ip, sizeof(ip), "%d.%d.%d.%d", a, b, c, d);
    resolve_discovered_label(ip, label, (int)(sizeof(label) / sizeof(label[0])));

    EnterCriticalSection(&g_scan_lock);
    if (g_discovered_count >= MAX_DISCOVERED) {
        LeaveCriticalSection(&g_scan_lock);
        return;
    }
    host = &g_discovered[g_discovered_count];
    lstrcpynA(host->ip, ip, (int)sizeof(host->ip));
    lstrcpynW(host->label, label, (int)(sizeof(host->label) / sizeof(host->label[0])));
    g_discovered_count++;
    LeaveCriticalSection(&g_scan_lock);
}

static DWORD WINAPI lan_scan_worker_thread(void* arg) {
    ScanState* scan = (ScanState*)arg;
    HANDLE icmp;
    char payload = 0;
    BYTE reply[sizeof(ICMP_ECHO_REPLY) + 32];

    icmp = IcmpCreateFile();
    if (icmp != INVALID_HANDLE_VALUE) {
        for (;;) {
            int host = (int)InterlockedIncrement(&scan->next_host);
            DWORD ip;
            DWORD replies;
            if (host > 254) break;
            ip = ipaddr_from_octets(scan->prefix[0], scan->prefix[1], scan->prefix[2], host);
            replies = IcmpSendEcho(icmp, ip, &payload, sizeof(payload), NULL,
                                   reply, sizeof(reply), 60);
            if (replies > 0) {
                add_discovered_ip(scan->prefix[0], scan->prefix[1], scan->prefix[2], host);
                PostMessageW(g_hwnd, WM_SCAN_DONE, 0, 0);
            }
        }
        IcmpCloseHandle(icmp);
    }

    if (InterlockedDecrement(&scan->active_workers) == 0) {
        InterlockedExchange(&g_scan_running, 0);
        PostMessageW(g_hwnd, WM_SCAN_DONE, 0, 0);
        free(scan);
    }
    return 0;
}

static DWORD WINAPI lan_scan_thread(void* ignored) {
    ScanState* scan;
    int prefix[3];

    (void)ignored;
    EnterCriticalSection(&g_scan_lock);
    g_discovered_count = 0;
    LeaveCriticalSection(&g_scan_lock);

    if (!find_lan_prefix(prefix)) {
        InterlockedExchange(&g_scan_running, 0);
        PostMessageW(g_hwnd, WM_SCAN_DONE, 0, 0);
        return 0;
    }

    scan = (ScanState*)calloc(1, sizeof(*scan));
    if (!scan) {
        InterlockedExchange(&g_scan_running, 0);
        PostMessageW(g_hwnd, WM_SCAN_DONE, 0, 0);
        return 0;
    }
    scan->prefix[0] = prefix[0];
    scan->prefix[1] = prefix[1];
    scan->prefix[2] = prefix[2];
    scan->next_host = 0;
    scan->active_workers = SCAN_WORKERS;

    for (int i = 0; i < SCAN_WORKERS; i++) {
        HANDLE worker = CreateThread(NULL, 0, lan_scan_worker_thread, scan, 0, NULL);
        if (worker) {
            CloseHandle(worker);
        } else if (InterlockedDecrement(&scan->active_workers) == 0) {
            InterlockedExchange(&g_scan_running, 0);
            free(scan);
            break;
        }
    }
    PostMessageW(g_hwnd, WM_SCAN_DONE, 0, 0);
    return 0;
}

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

static void update_window_title(void) {
    WCHAR title[MAX_OSC + 16];
    TermSession* session = active_session();
    const WCHAR* label;
    if (!g_hwnd || !session) return;
    label = session->title[0] ? session->title : session->name;
    lstrcpynW(title, APP_NAME L" - ", MAX_OSC);
    lstrcpynW(title + lstrlenW(title), label,
              MAX_OSC - lstrlenW(title));
    SetWindowTextW(g_hwnd, title);
}

static void handle_osc(void) {
    int i = 0;
    int code = 0;

    g_osc_buf[g_osc_len] = 0;
    while (i < g_osc_len && g_osc_buf[i] >= L'0' && g_osc_buf[i] <= L'9') {
        code = code * 10 + (g_osc_buf[i] - L'0');
        i++;
    }
    if (i >= g_osc_len || g_osc_buf[i] != L';') return;
    i++;

    if ((code == 0 || code == 2) && g_osc_buf[i]) {
        lstrcpynW(g_session->title, g_osc_buf + i,
                  (int)(sizeof(g_session->title) / sizeof(g_session->title[0])));
        if (g_session == active_session()) update_window_title();
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

static void finish_password_capture(void) {
    HostEntry* host;
    if (!g_session || !g_session->password_capture) return;
    g_session->password_capture = 0;

    if (g_session->host_index >= 0 && g_session->host_index < g_host_count &&
        g_session->password_input_len > 0) {
        host = &g_hosts[g_session->host_index];
        g_session->password_input[g_session->password_input_len] = 0;
        if (protect_password(g_session->password_input, host->password_blob,
                             HOST_PASSWORD_BLOB_MAX)) {
            save_hosts();
        }
    }

    SecureZeroMemory(g_session->password_input, sizeof(g_session->password_input));
    g_session->password_input_len = 0;
}

static void capture_password_bytes(const char* bytes, int len) {
    if (!g_session || !g_session->password_capture || !bytes || len <= 0) return;
    for (int i = 0; i < len; i++) {
        if (bytes[i] == '\r' || bytes[i] == '\n') {
            finish_password_capture();
            return;
        }
        if (g_session->password_input_len < HOST_PASSWORD_MAX - 1) {
            g_session->password_input[g_session->password_input_len++] = bytes[i];
        }
    }
}

static void capture_password_backspace(void) {
    if (!g_session || !g_session->password_capture) return;
    if (g_session->password_input_len > 0) {
        g_session->password_input[--g_session->password_input_len] = 0;
    }
}

static void maybe_handle_password_prompt(TermSession* session, const char* bytes, DWORD count) {
    HostEntry* host;
    char password[HOST_PASSWORD_MAX];

    if (!session || session->host_index < 0 || session->host_index >= g_host_count) return;
    host = &g_hosts[session->host_index];
    if (!host->command[0]) return;

    for (DWORD i = 0; i < count; i++) {
        char c = bytes[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c < 32 && c != '\r' && c != '\n' && c != '\t') continue;
        if (session->prompt_tail_len >= (int)sizeof(session->prompt_tail) - 1) {
            memmove(session->prompt_tail, session->prompt_tail + 1,
                    sizeof(session->prompt_tail) - 2);
            session->prompt_tail_len = (int)sizeof(session->prompt_tail) - 2;
        }
        session->prompt_tail[session->prompt_tail_len++] = c;
        session->prompt_tail[session->prompt_tail_len] = 0;
    }

    if (!strstr(session->prompt_tail, "password:")) return;
    session->prompt_tail_len = 0;
    session->prompt_tail[0] = 0;

    if (host->password_blob[0] &&
        unprotect_password(host->password_blob, password, sizeof(password))) {
        if (session->backend.write) {
            session->backend.write(&session->backend, password, (DWORD)strlen(password));
            session->backend.write(&session->backend, "\r", 1);
        }
        SecureZeroMemory(password, sizeof(password));
    } else {
        session->password_capture = 1;
        session->password_input_len = 0;
        SecureZeroMemory(session->password_input, sizeof(session->password_input));
    }
}

static void feed_output(TermSession* session, const char* bytes, DWORD count) {
    TermSession* old_session = g_session;
    WCHAR wbuf[8192];
    int wlen;
    if (!session) return;
    g_session = session;
    maybe_handle_password_prompt(session, bytes, count);
    wlen = decode_output_bytes(bytes, count, wbuf, 8192);
    if (wlen <= 0) {
        g_session = old_session;
        return;
    }

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
    g_session = old_session;
    schedule_terminal_repaint();
}

static void get_terminal_rect(HWND hwnd, RECT* term) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    term->left = rc.left + HOST_PANEL_W + TERM_PAD;
    term->top = rc.top + TAB_BAR_H + TERM_PAD;
    term->right = rc.right - TERM_PAD;
    term->bottom = rc.bottom - TERM_PAD;
    if (term->right < term->left) term->right = term->left;
    if (term->bottom < term->top) term->bottom = term->top;
}

static int point_in_terminal(HWND hwnd, LPARAM lp) {
    RECT term;
    int px = (int)(short)LOWORD(lp);
    int py = (int)(short)HIWORD(lp);
    get_terminal_rect(hwnd, &term);
    return px >= term.left && px < term.right && py >= term.top && py < term.bottom;
}

static int host_at_point(int px, int py) {
    int row;
    if (px < 0 || px >= HOST_PANEL_W || py < 28) return -1;
    row = (py - 28) / HOST_ROW_H;
    return row >= 0 && row < g_host_count ? row : -1;
}

static int scan_row_y(void) {
    return 31 + g_host_count * HOST_ROW_H + 8;
}

static int point_is_scan_row(int px, int py) {
    int y = scan_row_y();
    return px >= 0 && px < HOST_PANEL_W && py >= y && py < y + HOST_ROW_H;
}

static int discovered_at_point(int px, int py) {
    int first_y = scan_row_y() + HOST_ROW_H + 24;
    int row;
    if (px < 0 || px >= HOST_PANEL_W || py < first_y) return -1;
    row = (py - first_y) / HOST_ROW_H;
    return row >= 0 && row < g_discovered_count ? row : -1;
}

static int tab_at_point(int px, int py) {
    int tab;
    if (py < 0 || py >= TAB_BAR_H || px < HOST_PANEL_W + 8) return -1;
    tab = (px - (HOST_PANEL_W + 8)) / TAB_W;
    return tab >= 0 && tab < g_session_count ? tab : -1;
}

static void handle_chrome_click(HWND hwnd, LPARAM lp) {
    int px = (int)(short)LOWORD(lp);
    int py = (int)(short)HIWORD(lp);
    int host = host_at_point(px, py);
    int tab = tab_at_point(px, py);
    int discovered = discovered_at_point(px, py);

    g_selection_active = 0;
    SetFocus(hwnd);
    if (tab >= 0) {
        switch_session(tab);
    } else if (host >= 0) {
        start_host_session(host);
    } else if (point_is_scan_row(px, py)) {
        start_lan_scan();
    } else if (discovered >= 0) {
        char command[HOST_COMMAND_MAX];
        snprintf(command, sizeof(command), "ssh %s", g_discovered[discovered].ip);
        start_command_session(g_discovered[discovered].label, command, -1);
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static void mouse_to_cell(LPARAM lp, int* cell_x, int* cell_y) {
    RECT term;
    int px = (int)(short)LOWORD(lp);
    int py = (int)(short)HIWORD(lp);
    int x;
    int y;

    get_terminal_rect(g_hwnd, &term);
    x = (px - term.left) / g_char_w;
    y = (py - term.top) / g_char_h;

    if (px < term.left) x = 0;
    if (py < term.top) y = 0;
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
    capture_password_bytes(utf8, utf8_len);
    backend_write(utf8, (DWORD)utf8_len);
    free(utf8);
    return 1;
}

static void post_backend_output(const char* data, DWORD len, void* user) {
    TermSession* session = (TermSession*)user;
    BackendData* msg;

    if (!data || len == 0) {
        PostMessageW(g_hwnd, WM_BACKEND_EXIT, 0, (LPARAM)session);
        return;
    }

    log_backend_output(data, len);
    msg = (BackendData*)malloc(sizeof(*msg) + len);
    if (!msg) return;
    msg->session = session;
    msg->len = len;
    memcpy(msg->data, data, len);
    if (!PostMessageW(g_hwnd, WM_BACKEND_DATA, 0, (LPARAM)msg)) {
        free(msg);
    }
}

static int backend_write(const char* s, DWORD len) {
    if (!g_session) return 0;
    if (!g_backend.write) return 0;
    return_to_live_view();
    return g_backend.write(&g_backend, s, len);
}

static int session_for_host(int host_index) {
    for (int i = 0; i < g_session_count; i++) {
        if (g_sessions[i] && g_sessions[i]->host_index == host_index) return i;
    }
    return -1;
}

static int session_is_live(TermSession* session) {
    for (int i = 0; i < g_session_count; i++) {
        if (g_sessions[i] == session) return 1;
    }
    return 0;
}

static void switch_session(int index) {
    if (index < 0 || index >= g_session_count || !g_sessions[index]) return;
    set_active_session(index);
    g_selection_active = 0;
    update_window_title();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void switch_relative_session(int delta) {
    int next;
    if (g_session_count <= 1 || g_active_session < 0) return;
    next = (g_active_session + delta + g_session_count) % g_session_count;
    switch_session(next);
}

static DWORD WINAPI cleanup_session_thread(void* arg) {
    TermSession* session = (TermSession*)arg;
    if (!session) return 0;
    if (session->backend.stop) session->backend.stop(&session->backend);
    free(session);
    return 0;
}

static void close_session(int index) {
    TermSession* session;
    HANDLE cleanup;
    int old_active = g_active_session;
    if (index < 0 || index >= g_session_count || !g_sessions[index]) return;

    session = g_sessions[index];
    session->exited = 1;

    for (int i = index; i < g_session_count - 1; i++) {
        g_sessions[i] = g_sessions[i + 1];
    }
    g_session_count--;
    g_sessions[g_session_count] = NULL;

    if (g_session_count == 0) {
        set_active_session(-1);
    } else if (old_active == index) {
        if (index >= g_session_count) index = g_session_count - 1;
        set_active_session(index);
    } else if (old_active > index) {
        set_active_session(old_active - 1);
    } else {
        set_active_session(old_active);
    }

    cleanup = CreateThread(NULL, 0, cleanup_session_thread, session, 0, NULL);
    if (cleanup) CloseHandle(cleanup);

    g_selection_active = 0;
    update_window_title();
    InvalidateRect(g_hwnd, NULL, FALSE);

    if (g_session_count == 0) {
        PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
}

static int start_command_session(const WCHAR* name, const char* command, int host_index) {
    TermSession* session;
    if (g_session_count >= MAX_SESSIONS) {
        die_box(L"Maximum session count reached.");
        return 0;
    }

    session = (TermSession*)calloc(1, sizeof(*session));
    if (!session) return 0;
    session->host_index = host_index;
    session->cursor_visible = 1;
    lstrcpynW(session->name, name,
              (int)(sizeof(session->name) / sizeof(session->name[0])));

    g_sessions[g_session_count] = session;
    g_session_count++;
    set_active_session(g_session_count - 1);
    clear_screen();

    if (!term_backend_conpty_init(&session->backend)) {
        die_box(L"Could not initialize the local ConPTY backend.");
        g_session_count--;
        g_sessions[g_session_count] = NULL;
        free(session);
        set_active_session(g_session_count - 1);
        return 0;
    }
    if (!session->backend.start(&session->backend, g_cols, g_rows,
                                post_backend_output, session)) {
        die_box(L"Could not start cmd.exe through ConPTY. This prototype needs Windows 10 1809 or newer.");
        if (session->backend.stop) session->backend.stop(&session->backend);
        g_session_count--;
        g_sessions[g_session_count] = NULL;
        free(session);
        set_active_session(g_session_count - 1);
        return 0;
    }
    if (command && command[0]) {
        session->backend.write(&session->backend, command, (DWORD)strlen(command));
        session->backend.write(&session->backend, "\r", 1);
    }
    update_window_title();
    InvalidateRect(g_hwnd, NULL, FALSE);
    return 1;
}

static int start_host_session(int host_index) {
    int existing = session_for_host(host_index);

    if (existing >= 0) {
        switch_session(existing);
        return 1;
    }
    if (host_index < 0 || host_index >= g_host_count) return 0;
    return start_command_session(g_hosts[host_index].name,
                                 g_hosts[host_index].command,
                                 host_index);
}

static void start_lan_scan(void) {
    HANDLE thread;
    if (InterlockedCompareExchange(&g_scan_running, 1, 0) != 0) return;
    g_discovered_count = 0;
    thread = CreateThread(NULL, 0, lan_scan_thread, NULL, 0, NULL);
    if (thread) {
        CloseHandle(thread);
    } else {
        InterlockedExchange(&g_scan_running, 0);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
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
    RECT term;
    int cols;
    int rows;

    get_terminal_rect(hwnd, &term);
    cols = (term.right - term.left) / g_char_w;
    rows = (term.bottom - term.top) / g_char_h;
    if (cols < 20) cols = 20;
    if (rows < 8) rows = 8;
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    if (cols == g_cols && rows == g_rows) return;

    EnterCriticalSection(&g_lock);
    g_cols = cols;
    g_rows = rows;
    for (int i = 0; i < g_session_count; i++) {
        TermSession* old_session = g_session;
        g_session = g_sessions[i];
        if (!g_session) {
            g_session = old_session;
            continue;
        }
        if (g_cx >= g_cols) g_cx = g_cols - 1;
        if (g_cy >= g_rows) g_cy = g_rows - 1;
        if (g_backend.resize) {
            g_backend.resize(&g_backend, g_cols, g_rows);
        }
        g_session = old_session;
    }
    LeaveCriticalSection(&g_lock);
}

static void paint_chrome(HWND hwnd, HDC dc, const RECT* term) {
    RECT rc;
    RECT side;
    RECT line;

    GetClientRect(hwnd, &rc);
    SetRect(&side, rc.left, rc.top, HOST_PANEL_W, rc.bottom);
    fill_solid_rect(dc, &rc, RGB(10, 13, 17));
    fill_solid_rect(dc, &side, RGB(16, 19, 24));
    fill_solid_rect(dc, term, DEFAULT_BG);

    SetRect(&line, HOST_PANEL_W, rc.top, HOST_PANEL_W + 1, rc.bottom);
    fill_solid_rect(dc, &line, RGB(43, 50, 58));
    SetRect(&line, HOST_PANEL_W, TAB_BAR_H, rc.right, TAB_BAR_H + 1);
    fill_solid_rect(dc, &line, RGB(43, 50, 58));

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(155, 166, 178));
    TextOutW(dc, 10, 5, L"Hosts", 5);

    for (int i = 0; i < g_host_count; i++) {
        if (session_for_host(i) >= 0) {
            SetTextColor(dc, RGB(232, 238, 245));
        } else if (g_hosts[i].command[0]) {
            SetTextColor(dc, RGB(200, 210, 220));
        } else {
            SetTextColor(dc, RGB(118, 128, 138));
        }
        TextOutW(dc, 14, 31 + i * HOST_ROW_H,
                 g_hosts[i].name, lstrlenW(g_hosts[i].name));
    }

    {
        int y = scan_row_y();
        const WCHAR* scan_label = InterlockedCompareExchange(&g_scan_running, 0, 0)
                                  ? L"Scan LAN..." : L"Scan LAN";
        SetTextColor(dc, RGB(200, 210, 220));
        TextOutW(dc, 14, y, scan_label, lstrlenW(scan_label));

        y += HOST_ROW_H + 8;
        SetTextColor(dc, RGB(155, 166, 178));
        TextOutW(dc, 10, y, L"Discovered", 10);
        y += HOST_ROW_H;
        for (int i = 0; i < g_discovered_count; i++) {
            RECT row;
            SetRect(&row, 14, y + i * HOST_ROW_H, HOST_PANEL_W - 8,
                    y + (i + 1) * HOST_ROW_H);
            SetTextColor(dc, RGB(180, 190, 200));
            DrawTextW(dc, g_discovered[i].label, -1, &row,
                      DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
        }
    }

    for (int i = 0; i < g_session_count; i++) {
        if (!g_sessions[i]) continue;
        SetTextColor(dc, i == g_active_session ? RGB(232, 238, 245) : RGB(145, 155, 165));
        TextOutW(dc, HOST_PANEL_W + 12 + i * TAB_W, 5,
                 g_sessions[i]->name, lstrlenW(g_sessions[i]->name));
    }
}

static void paint_terminal(HWND hwnd, HDC dc) {
    HFONT old_font;
    RECT term;

    get_terminal_rect(hwnd, &term);
    old_font = (HFONT)SelectObject(dc, g_font);
    paint_chrome(hwnd, dc, &term);
    if (!g_session) {
        SelectObject(dc, old_font);
        return;
    }
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
            TextOutW(dc, term.left + x * g_char_w, term.top + y * g_char_h, &ch, 1);
        }
    }

    if (g_scroll_offset == 0 && g_cursor_visible) {
        RECT cursor = {
            term.left + g_cx * g_char_w,
            term.top + g_cy * g_char_h,
            term.left + (g_cx + 1) * g_char_w,
            term.top + (g_cy + 1) * g_char_h
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
        if (g_app_icon) {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_app_icon);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_app_icon);
        }
        g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
        if (!g_font) {
            g_font = (HFONT)GetStockObject(ANSI_FIXED_FONT);
        }
        update_metrics(hwnd);
        resize_grid(hwnd);
        open_output_log();
        if (!start_host_session(0)) {
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
            finish_password_capture();
            backend_write("\r", 1);
        } else if (wp >= 32) {
            WCHAR wc = (WCHAR)wp;
            char utf8[8];
            int len = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, utf8, sizeof(utf8), NULL, NULL);
            if (len > 0) {
                capture_password_bytes(utf8, len);
                backend_write(utf8, (DWORD)len);
            }
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
                close_session(g_active_session);
                return 0;
            }
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) && wp == VK_TAB) {
            switch_relative_session((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            return 0;
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
        if (wp == VK_BACK) {
            capture_password_backspace();
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
        if (!point_in_terminal(hwnd, lp)) {
            handle_chrome_click(hwnd, lp);
            return 0;
        }
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
        if (!point_in_terminal(hwnd, lp)) {
            handle_chrome_click(hwnd, lp);
            return 0;
        }
        mouse_to_cell(lp, &x, &y);
        start_word_selection_at(x, y);
        return 0;
    }

    case WM_RBUTTONDOWN:
        if (point_in_terminal(hwnd, lp)) {
            paste_clipboard_text();
        }
        return 0;

    case WM_BACKEND_DATA:
        if (lp) {
            BackendData* data = (BackendData*)lp;
            if (session_is_live(data->session)) {
                feed_output(data->session, data->data, data->len);
            }
        }
        free((void*)lp);
        return 0;

    case WM_BACKEND_EXIT:
        if (lp && session_is_live((TermSession*)lp)) {
            ((TermSession*)lp)->exited = 1;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;

    case WM_SCAN_DONE:
        InvalidateRect(hwnd, NULL, FALSE);
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
        for (int i = 0; i < g_session_count; i++) {
            if (g_sessions[i]) {
                g_session = g_sessions[i];
                if (g_backend.stop) g_backend.stop(&g_backend);
                free(g_sessions[i]);
                g_sessions[i] = NULL;
            }
        }
        g_session_count = 0;
        set_active_session(-1);
        close_output_log();
        if (g_font) DeleteObject(g_font);
        if (g_app_icon) {
            DestroyIcon(g_app_icon);
            g_app_icon = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    (void)prev;
    (void)cmdline;
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_scan_lock);
    {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    }
    g_app_icon = create_app_icon();
    load_hosts();

    WNDCLASSW wc = {0};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_IBEAM);
    wc.hIcon = g_app_icon ? g_app_icon : LoadIcon(NULL, IDI_APPLICATION);
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
        DeleteCriticalSection(&g_scan_lock);
        WSACleanup();
        return (int)msg.wParam;
    }
}
