// guest_stdio.cpp — the C stdio open family: fopen, freopen, fdopen, fopen_s and freopen_s.
//
// The guest's mode string follows its libc's (Dinkumware `_Foprep`) grammar, not the host's: parse
// it once and hand the host a spelling it accepts (guest_fopen_mode.hpp — on Windows a verbatim "r"
// opened in TEXT mode). Reached only by titles that ship no libc.prx; see that header's scope note.
// All five parse through it, so no two of them can read one mode differently (#4348), and every
// path goes through the same mount table as open(2) (guest_stream_host_path, hle_file.cpp). The
// rest of the stream surface — fread, fwrite, fseek, fclose, ... — is still in hle_file.cpp.
#include "hle/fs/guest_stdio.hpp"
#include "hle/dispatch/dispatch.hpp"
#include "hle/dispatch/nid.hpp"
#include "hle/fs/guest_devices.hpp"
#include "hle/fs/guest_fopen_mode.hpp"
#include "hle/fs/guest_sync.hpp"
#include "hle/kernel/sce_errno.hpp"
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <string>

namespace prosper {

#define HLE(name)                                                                                  \
    static PROSPER_SYSV_ABI uint64_t name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,      \
                                          uint64_t a4, uint64_t a5)
#define P(x) ((void*)(uintptr_t)(x))
#define CS(x) ((const char*)(uintptr_t)(x))

static FILE* open_stream(const char* path, const char* mode) {
    const GuestFopenMode m = parse_guest_fopen_mode(mode, kHostFopenDialect);
    if (!m.valid) {
        if (guest_file_log_enabled())
            fprintf(stderr, "[file] fopen mode='%s' -> EINVAL\n", mode ? mode : "(null)");
        errno = EINVAL;
        return nullptr;
    }
    std::string h;
    if (const int error = guest_stream_host_path(path, m.create, h)) {
        errno = error;
        return nullptr;
    }
    FILE* f = fopen(h.c_str(), m.host);
    guest_sync_note_stream(f, m.writable);
    if (guest_file_log_enabled())
        fprintf(stderr, "[file] fopen host='%s' mode='%s' (host '%s') -> %p error=%d\n", h.c_str(),
                mode, m.host, (void*)f, f ? 0 : errno);
    return f;
}

// freopen (C11 7.21.5.4p4) "first attempts to close any file that is associated with the specified
// stream", so once the stream is accepted every failure leaves it closed, whatever refused the new
// open: the mode, the path, or the host. The guest must not use the stream after a NULL return.
static FILE* reopen_stream(const char* path, const char* mode, FILE* stream) {
    if (!stream) {
        // prosper's choice: C leaves a null stream undefined, and the guest libc would dereference
        // it. Refuse without touching anything, as fopen refuses a null mode.
        errno = EINVAL;
        return nullptr;
    }
    if (!path) {
        // A null path asks to change the open stream's mode in place, and which changes are
        // permitted is implementation-defined (C11 7.21.5.4p3). prosper permits none: the
        // Microsoft CRT has no such reopen, and nothing read from the guest says which changes its
        // libc allows. Refuse with POSIX's errno for a mode the stream cannot take, leave the
        // stream open and usable rather than close a stream the guest may go on using, and say so
        // on every host so a title that relies on it is visible. CONFIDENCE: LOW.
        fprintf(stderr, "[file] freopen(NULL, '%s', %p): mode change not supported -> EBADF\n",
                mode ? mode : "(null)", (void*)stream);
        errno = EBADF;
        return nullptr;
    }
    const GuestFopenMode m = parse_guest_fopen_mode(mode, kHostFopenDialect);
    std::string h;
    const int error = !m.valid ? EINVAL : guest_stream_host_path(path, m.create, h);
    if (error) {
        guest_sync_close_stream(stream);
        if (guest_file_log_enabled())
            fprintf(stderr, "[file] freopen path='%s' mode='%s' -> closed, error=%d\n", path,
                    mode ? mode : "(null)", error);
        errno = error;
        return nullptr;
    }
    FILE* f = guest_sync_reopen_stream(h.c_str(), m.host, stream, m.writable);
    if (guest_file_log_enabled())
        fprintf(stderr, "[file] freopen host='%s' mode='%s' (host '%s') -> %p error=%d\n",
                h.c_str(), mode, m.host, (void*)f, f ? 0 : errno);
    return f;
}

HLE(f_fopen) {
    return (uint64_t)(uintptr_t)open_stream(CS(a0), CS(a1));
}
HLE(f_freopen) {
    return (uint64_t)(uintptr_t)reopen_stream(CS(a0), CS(a1), (FILE*)P(a2));
}

// A guest descriptor is the host descriptor f_open returned, so the host's fdopen applies once the
// mode is read by fopen's grammar. 'w' does not truncate and nothing is created: the descriptor is
// already open, which is also why the exclusive letter is dropped from the host spelling (hosts
// differ on whether fdopen accepts it at all, and it has nothing left to act on). Whether the mode
// must agree with the descriptor's access mode is the host's answer — glibc and Darwin refuse a
// mismatch with EINVAL, the Microsoft CRT does not check. CONFIDENCE: MED (C/POSIX contract; the
// guest libc's fdopen was not read).
HLE(f_fdopen) {
    const char* mode = CS(a1);
    const GuestFopenMode m = parse_guest_fopen_mode(mode, kHostFopenDialect);
    if (!m.valid) {
        errno = EINVAL;
        return 0;
    }
    if (a0 > INT_MAX) {   // also every negative descriptor
        errno = EBADF;
        return 0;
    }
    const int fd = (int)a0;
    if (guest_device_is_fd(fd)) {
        // As fopen: a host stream would read the device's null backing object, not entropy.
        errno = ENODEV;
        return 0;
    }
    char host_mode[sizeof m.host] = {};
    char* out = host_mode;
    for (const char* p = m.host; *p; ++p)
        if (*p != 'x') *out++ = *p;
    FILE* f = guest_fd_stream(fd, host_mode, m.writable);
    if (guest_file_log_enabled())
        fprintf(stderr, "[file] fdopen fd=%d mode='%s' (host '%s') -> %p error=%d\n", fd, mode,
                host_mode, (void*)f, f ? 0 : errno);
    return (uint64_t)(uintptr_t)f;
}

// The C11 Annex K opens (K.3.5.2.1 fopen_s, K.3.5.2.2 freopen_s): the stream comes back through an
// out-pointer and the result is an errno value, zero on success; on failure the out-pointer is set
// to NULL. A runtime-constraint violation — a null out-pointer, mode or stream, or for fopen_s a
// null path — opens and closes nothing, stores NULL through a non-null out-pointer and returns
// EINVAL. freopen_s's path may be null (the mode change freopen refuses above). The result is the
// FreeBSD number the guest compares against; the errno slot is left as fopen leaves it.
// CONFIDENCE: MED — the published Annex K contract; the guest libc's own fopen_s/freopen_s were
// not read, so its runtime-constraint handler (which Annex K allows to abort) is not modelled, and
// Annex K's 'u' mode prefix, not a `_Foprep` letter, is refused EINVAL like any other.
static uint64_t annex_k_status(const FILE* stream) {
    if (stream) return 0;
    return static_cast<uint64_t>(hle::freebsd_errno_from_host(errno, hle::FreeBsdErrno::EIo));
}
static uint64_t annex_k_constraint_violation(FILE** out) {
    if (out) *out = nullptr;
    errno = EINVAL;
    return static_cast<uint64_t>(hle::FreeBsdErrno::EInval);
}
HLE(f_fopen_s) {
    FILE** out = (FILE**)P(a0);
    if (!out || !a1 || !a2) return annex_k_constraint_violation(out);
    FILE* f = open_stream(CS(a1), CS(a2));
    const uint64_t status = annex_k_status(f);
    *out = f;
    return status;
}
HLE(f_freopen_s) {
    FILE** out = (FILE**)P(a0);
    if (!out || !a2 || !a3) return annex_k_constraint_violation(out);
    FILE* f = reopen_stream(CS(a1), CS(a2), (FILE*)P(a3));
    const uint64_t status = annex_k_status(f);
    *out = f;
    return status;
}

void register_guest_stdio_hle() {
#define R(str, fn) Hle::register_fn(nid_hash(str), (HleFn)(fn), str)
    R("fopen", f_fopen);
    // #4348: these four were the return-0 stub (a NULL FILE*, or "success" with nothing written).
    R("freopen", f_freopen);
    R("fdopen", f_fdopen);
    R("fopen_s", f_fopen_s);
    R("freopen_s", f_freopen_s);
#undef R
}

}   // namespace prosper
