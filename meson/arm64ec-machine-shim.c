/*
 * arm64ec-machine-shim.c
 *
 * A transparent wrapper around MSVC's lib.exe / link.exe that rewrites a
 * "/MACHINE:ARM64" argument to "/MACHINE:ARM64EC" before forwarding.
 *
 * Meson has no ARM64EC machine.  For an aarch64 cross build it derives the
 * archiver/linker "/MACHINE" from the cross file's cpu_family and emits
 * "/MACHINE:ARM64", which lib/link then reject for /arm64EC objects
 * (fatal error LNK1392 / LNK1112).  Pointing meson's `ar` / `c_ld` / `cpp_ld`
 * at this shim supplies the correct "/MACHINE:ARM64EC" without patching meson.
 *
 * "/MACHINE:ARM64" can appear either directly on the command line (short
 * links, and lib's own output arg) or inside a response file (@file, used for
 * long links), so the shim rewrites both.  Response files are ASCII, so the
 * in-file rewrite is done byte-wise on a temporary copy.
 *
 * The lib shim also answers meson's "--version" archiver probe by forwarding
 * "/?": meson classifies an archiver whose basename is not "lib(.exe)" as MSVC
 * only if the probe output contains "/OUT:", which "lib /?" prints but
 * "lib --version" does not.
 *
 * (The LLVM CMake build needs no shim: CMake detects _M_ARM64EC from the
 * /arm64EC compiler flag and emits /machine:ARM64EC natively.)
 *
 * Build both views from this one source (see build.cmd):
 *   cl /DSHIM_LIB  /Fe:arm64ec-lib.exe  arm64ec-machine-shim.c
 *   cl /DSHIM_LINK /Fe:arm64ec-link.exe arm64ec-machine-shim.c
 *
 * The shim finds the real tool by name on PATH -- its own name is distinct,
 * so it never recurses.
 */

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "shell32.lib")

#if defined(SHIM_LIB)
#  define REAL_TOOL L"lib.exe"
#elif defined(SHIM_LINK)
#  define REAL_TOOL L"link.exe"
#else
#  error "define SHIM_LIB or SHIM_LINK"
#endif

#define MACHINE_TOKEN   "/MACHINE:ARM64"
#define MACHINE_REPLACE "/MACHINE:ARM64EC"

static int is_delim(char c)
{
    return c == 0 || c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/*
 * Copy src (len bytes) to a freshly allocated buffer, replacing every
 * standalone "/MACHINE:ARM64" with "/MACHINE:ARM64EC".  Returns the new buffer
 * and its length via *out_len; caller frees.
 */
static char *rewrite_machine_bytes(const char *src, size_t len, size_t *out_len)
{
    const size_t tl = sizeof(MACHINE_TOKEN) - 1;
    const size_t rl = sizeof(MACHINE_REPLACE) - 1;
    char *dst = HeapAlloc(GetProcessHeap(), 0, len + (len / tl + 1) * (rl - tl) + 1);
    if (!dst) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; ) {
        if (i + tl <= len && _strnicmp(src + i, MACHINE_TOKEN, tl) == 0 &&
            is_delim(i + tl < len ? src[i + tl] : 0)) {
            memcpy(dst + o, MACHINE_REPLACE, rl);
            o += rl;
            i += tl;
        } else {
            dst[o++] = src[i++];
        }
    }
    *out_len = o;
    return dst;
}

/*
 * Rewrite a response file's "/MACHINE:ARM64" into a temp copy.  On success
 * writes the temp path to out (MAX_PATH wide chars) and returns 1.
 */
static int rewrite_rsp(const wchar_t *path, wchar_t *out)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD sz = GetFileSize(h, NULL);
    char *buf = HeapAlloc(GetProcessHeap(), 0, sz ? sz : 1);
    DWORD rd = 0;
    if (!buf || (sz && !ReadFile(h, buf, sz, &rd, NULL))) {
        CloseHandle(h);
        if (buf) HeapFree(GetProcessHeap(), 0, buf);
        return 0;
    }
    CloseHandle(h);

    size_t olen = 0;
    char *outbuf = rewrite_machine_bytes(buf, rd, &olen);
    HeapFree(GetProcessHeap(), 0, buf);
    if (!outbuf) return 0;

    wchar_t dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, dir) == 0 ||
        GetTempFileNameW(dir, L"ecr", 0, out) == 0) {
        HeapFree(GetProcessHeap(), 0, outbuf);
        return 0;
    }
    HANDLE ho = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, NULL);
    if (ho == INVALID_HANDLE_VALUE) {
        HeapFree(GetProcessHeap(), 0, outbuf);
        return 0;
    }
    DWORD wr = 0;
    WriteFile(ho, outbuf, (DWORD)olen, &wr, NULL);
    CloseHandle(ho);
    HeapFree(GetProcessHeap(), 0, outbuf);
    return 1;
}

/* Append arg to out (n = current length) with Windows command-line quoting. */
static void append_quoted(wchar_t *out, size_t *n, const wchar_t *arg)
{
    int need = (*arg == 0);
    for (const wchar_t *p = arg; *p && !need; p++)
        if (*p == L' ' || *p == L'\t' || *p == L'"' || *p == L'\n' || *p == L'\v')
            need = 1;
    if (!need) {
        for (const wchar_t *p = arg; *p; p++) out[(*n)++] = *p;
        return;
    }
    out[(*n)++] = L'"';
    for (const wchar_t *p = arg;; p++) {
        size_t bs = 0;
        while (*p == L'\\') { bs++; p++; }
        if (*p == 0) {
            for (size_t k = 0; k < bs * 2; k++) out[(*n)++] = L'\\';
            break;
        } else if (*p == L'"') {
            for (size_t k = 0; k < bs * 2 + 1; k++) out[(*n)++] = L'\\';
            out[(*n)++] = L'"';
        } else {
            for (size_t k = 0; k < bs; k++) out[(*n)++] = L'\\';
            out[(*n)++] = *p;
        }
    }
    out[(*n)++] = L'"';
}

int main(void)
{
    wchar_t real[MAX_PATH];
    if (SearchPathW(NULL, REAL_TOOL, NULL, MAX_PATH, real, NULL) == 0) {
        fwprintf(stderr, L"[arm64ec-shim] cannot find %s on PATH\n", REAL_TOOL);
        return 9009;
    }

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    /* Track temp response files so we can delete them after the child exits. */
    wchar_t **tmps = HeapAlloc(GetProcessHeap(), 0, sizeof(wchar_t *) * (argc + 1));
    int ntmp = 0;

    /* Command line: "<real>" then each transformed, requoted argument.  Size
     * generously -- the machine rewrite grows an arg by at most 2 chars, and a
     * temp path is bounded by MAX_PATH. */
    size_t cap = wcslen(real) + 4;
    for (int i = 1; i < argc; i++) cap += wcslen(argv[i]) * 2 + MAX_PATH + 4;
    wchar_t *cmd = HeapAlloc(GetProcessHeap(), 0, cap * sizeof(wchar_t));
    if (!cmd || !tmps) return 1;

    size_t n = 0;
    cmd[n++] = L'"';
    for (const wchar_t *r = real; *r; r++) cmd[n++] = *r;
    cmd[n++] = L'"';

    for (int i = 1; i < argc; i++) {
        wchar_t *a = argv[i];
        cmd[n++] = L' ';

        if (_wcsicmp(a, L"" MACHINE_TOKEN) == 0) {
            append_quoted(cmd, &n, L"" MACHINE_REPLACE);
            continue;
        }
#if defined(SHIM_LIB)
        if (_wcsicmp(a, L"--version") == 0) {
            append_quoted(cmd, &n, L"/?");
            continue;
        }
#endif
        if (a[0] == L'@') {
            wchar_t tmp[MAX_PATH];
            if (rewrite_rsp(a + 1, tmp)) {
                tmps[ntmp++] = _wcsdup(tmp);
                cmd[n++] = L'@';
                append_quoted(cmd, &n, tmp);
                continue;
            }
        }
        append_quoted(cmd, &n, a);
    }
    cmd[n] = 0;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    DWORD code = 1;
    if (!CreateProcessW(real, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        fwprintf(stderr, L"[arm64ec-shim] failed to launch %s (err %lu)\n",
                 real, GetLastError());
    } else {
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    for (int i = 0; i < ntmp; i++) {
        DeleteFileW(tmps[i]);
        free(tmps[i]);
    }
    return (int)code;
}
