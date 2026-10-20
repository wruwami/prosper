// guest_stdio.cpp — the C stdio open family: fopen.
//
// The guest's mode string follows its libc's (Dinkumware `_Foprep`) grammar, not the host's: parse
// it once and hand the host a spelling it accepts (guest_fopen_mode.hpp — on Windows a verbatim "r"
// opened in TEXT mode). Reached only by titles that ship no libc.prx; see that header's scope note.
// The path goes through the same mount table as open(2) (guest_stream_host_path, hle_file.cpp). The
// rest of the stream surface — fread, fwrite, fseek, fclose, ... — is still in hle_file.cpp.
#include "hle/fs/guest_stdio.hpp"
#include "hle/dispatch/dispatch.hpp"
#include "hle/dispatch/nid.hpp"
#include "hle/fs/guest_fopen_mode.hpp"
#include "hle/fs/guest_sync.hpp"
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <string>

namespace prosper {

#define HLE(name)                                                                                  \
    static PROSPER_SYSV_ABI uint64_t name(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,      \
                                          uint64_t a4, uint64_t a5)
#define CS(x) ((const char*)(uintptr_t)(x))

HLE(f_fopen) {
    const char* mode = CS(a1);
    const GuestFopenMode m = parse_guest_fopen_mode(mode, kHostFopenDialect);
    if (!m.valid) {
        if (guest_file_log_enabled())
            fprintf(stderr, "[file] fopen mode='%s' -> EINVAL\n", mode ? mode : "(null)");
        errno = EINVAL;
        return 0;
    }
    std::string h;
    if (const int error = guest_stream_host_path(CS(a0), m.create, h)) {
        errno = error;
        return 0;
    }
    FILE* f = fopen(h.c_str(), m.host);
    guest_sync_note_stream(f, m.writable);
    if (guest_file_log_enabled())
        fprintf(stderr, "[file] fopen host='%s' mode='%s' (host '%s') -> %p error=%d\n", h.c_str(),
                mode, m.host, (void*)f, f ? 0 : errno);
    return (uint64_t)(uintptr_t)f;
}

void register_guest_stdio_hle() {
#define R(str, fn) Hle::register_fn(nid_hash(str), (HleFn)(fn), str)
    R("fopen", f_fopen);
#undef R
}

}   // namespace prosper
