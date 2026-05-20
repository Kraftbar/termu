#ifndef TERM_BACKEND_H
#define TERM_BACKEND_H

#define _WIN32_WINNT 0x0A00
#include <windows.h>

typedef struct TermBackend TermBackend;

typedef void (*TermBackendOutputFn)(const char* data, DWORD len, void* user);

struct TermBackend {
    const WCHAR* name;
    void* state;
    int (*start)(TermBackend* backend, int cols, int rows,
                 TermBackendOutputFn output, void* output_user);
    int (*write)(TermBackend* backend, const char* data, DWORD len);
    void (*resize)(TermBackend* backend, int cols, int rows);
    void (*stop)(TermBackend* backend);
};

int term_backend_conpty_init(TermBackend* backend);

#endif
