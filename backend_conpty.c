#define _WIN32_WINNT 0x0A00
#include "term_backend.h"

#include <stdlib.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

typedef VOID* TermuHPCON;
typedef HRESULT (WINAPI *CreatePseudoConsoleFn)(COORD, HANDLE, HANDLE, DWORD, TermuHPCON*);
typedef HRESULT (WINAPI *ResizePseudoConsoleFn)(TermuHPCON, COORD);
typedef VOID (WINAPI *ClosePseudoConsoleFn)(TermuHPCON);

typedef struct {
    HANDLE pty_in;
    HANDLE pty_out;
    HANDLE child;
    HANDLE reader_thread;
    TermuHPCON hpc;
    CreatePseudoConsoleFn create_pseudo_console;
    ResizePseudoConsoleFn resize_pseudo_console;
    ClosePseudoConsoleFn close_pseudo_console;
    TermBackendOutputFn output;
    void* output_user;
    volatile LONG stopping;
} ConptyState;

static void close_handle(HANDLE* handle) {
    if (*handle) {
        CloseHandle(*handle);
        *handle = NULL;
    }
}

static int load_conpty(ConptyState* state) {
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel) return 0;
    state->create_pseudo_console =
        (CreatePseudoConsoleFn)GetProcAddress(kernel, "CreatePseudoConsole");
    state->resize_pseudo_console =
        (ResizePseudoConsoleFn)GetProcAddress(kernel, "ResizePseudoConsole");
    state->close_pseudo_console =
        (ClosePseudoConsoleFn)GetProcAddress(kernel, "ClosePseudoConsole");
    return state->create_pseudo_console &&
           state->resize_pseudo_console &&
           state->close_pseudo_console;
}

static void conpty_cleanup_process_attrs(LPPROC_THREAD_ATTRIBUTE_LIST attrs) {
    if (attrs) {
        DeleteProcThreadAttributeList(attrs);
        HeapFree(GetProcessHeap(), 0, attrs);
    }
}

static void conpty_close_resources(ConptyState* state) {
    InterlockedExchange(&state->stopping, 1);

    if (state->pty_in) {
        DWORD written = 0;
        WriteFile(state->pty_in, "exit\r", 5, &written, NULL);
    }

    if (state->child) {
        DWORD wait = WaitForSingleObject(state->child, 800);
        if (wait == WAIT_TIMEOUT) {
            TerminateProcess(state->child, 0);
            WaitForSingleObject(state->child, 800);
        }
    }

    close_handle(&state->pty_in);
    close_handle(&state->pty_out);

    if (state->hpc && state->close_pseudo_console) {
        state->close_pseudo_console(state->hpc);
        state->hpc = NULL;
    }

    if (state->reader_thread) {
        WaitForSingleObject(state->reader_thread, 500);
        close_handle(&state->reader_thread);
    }

    close_handle(&state->child);
}

static DWORD WINAPI conpty_reader_thread(void* arg) {
    ConptyState* state = (ConptyState*)arg;
    char buf[4096];
    DWORD read = 0;

    while (ReadFile(state->pty_out, buf, sizeof(buf), &read, NULL) && read > 0) {
        if (InterlockedCompareExchange(&state->stopping, 0, 0)) break;
        if (state->output) state->output(buf, read, state->output_user);
    }

    if (!InterlockedCompareExchange(&state->stopping, 0, 0) && state->output) {
        state->output(NULL, 0, state->output_user);
    }
    return 0;
}

static int build_cmdline(WCHAR* cmdline, DWORD chars) {
    DWORD len = GetEnvironmentVariableW(L"ComSpec", cmdline, chars);
    if (len > 0 && len < chars) return 1;
    if (chars < 8) return 0;
    lstrcpyW(cmdline, L"cmd.exe");
    return 1;
}

static int conpty_start(TermBackend* backend, int cols, int rows,
                        TermBackendOutputFn output, void* output_user) {
    ConptyState* state = (ConptyState*)backend->state;
    HANDLE in_read = NULL;
    HANDLE in_write = NULL;
    HANDLE out_read = NULL;
    HANDLE out_write = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    SIZE_T attr_size = 0;
    PROCESS_INFORMATION pi;
    STARTUPINFOEXW si;
    SECURITY_ATTRIBUTES sa;
    COORD size;
    HRESULT hr;
    WCHAR cmdline[MAX_PATH];

    ZeroMemory(&pi, sizeof(pi));
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&sa, sizeof(sa));

    if (!load_conpty(state)) return 0;

    state->output = output;
    state->output_user = output_user;
    InterlockedExchange(&state->stopping, 0);

    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&in_read, &in_write, &sa, 0)) goto fail;
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) goto fail;
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);

    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;
    hr = state->create_pseudo_console(size, in_read, out_write, 0, &state->hpc);
    close_handle(&in_read);
    close_handle(&out_write);
    if (FAILED(hr)) goto fail;

    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
    attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attr_size);
    if (!attrs) goto fail;
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) goto fail;
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   state->hpc, sizeof(state->hpc), NULL, NULL)) {
        goto fail;
    }

    si.StartupInfo.cb = sizeof(si);
    si.lpAttributeList = attrs;

    if (!build_cmdline(cmdline, MAX_PATH)) goto fail;
    if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE,
                        EXTENDED_STARTUPINFO_PRESENT, NULL, NULL,
                        &si.StartupInfo, &pi)) {
        goto fail;
    }

    conpty_cleanup_process_attrs(attrs);
    close_handle(&pi.hThread);

    state->child = pi.hProcess;
    pi.hProcess = NULL;
    state->pty_in = in_write;
    state->pty_out = out_read;
    in_write = NULL;
    out_read = NULL;
    state->reader_thread = CreateThread(NULL, 0, conpty_reader_thread, state, 0, NULL);
    if (!state->reader_thread) goto fail;
    return 1;

fail:
    conpty_cleanup_process_attrs(attrs);
    close_handle(&pi.hThread);
    close_handle(&pi.hProcess);
    close_handle(&in_read);
    close_handle(&in_write);
    close_handle(&out_read);
    close_handle(&out_write);
    conpty_close_resources(state);
    return 0;
}

static int conpty_write(TermBackend* backend, const char* data, DWORD len) {
    ConptyState* state = (ConptyState*)backend->state;
    DWORD written = 0;
    if (!state->pty_in) return 0;
    return WriteFile(state->pty_in, data, len, &written, NULL) && written == len;
}

static void conpty_resize(TermBackend* backend, int cols, int rows) {
    ConptyState* state = (ConptyState*)backend->state;
    COORD size;
    if (!state->hpc || !state->resize_pseudo_console) return;
    size.X = (SHORT)cols;
    size.Y = (SHORT)rows;
    state->resize_pseudo_console(state->hpc, size);
}

static void conpty_stop(TermBackend* backend) {
    ConptyState* state = (ConptyState*)backend->state;
    if (!state) return;
    conpty_close_resources(state);
    free(state);
    backend->state = NULL;
}

int term_backend_conpty_init(TermBackend* backend) {
    ConptyState* state;
    if (!backend) return 0;
    ZeroMemory(backend, sizeof(*backend));
    state = (ConptyState*)calloc(1, sizeof(*state));
    if (!state) return 0;

    backend->name = L"local-conpty";
    backend->state = state;
    backend->start = conpty_start;
    backend->write = conpty_write;
    backend->resize = conpty_resize;
    backend->stop = conpty_stop;
    return 1;
}
