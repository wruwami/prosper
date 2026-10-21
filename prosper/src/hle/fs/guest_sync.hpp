#pragma once
#include <cstdio>
#include <string>

namespace prosper {

// POSIX guest filesystem barrier. Retain one private descriptor per encountered filesystem
// until process exit, so closing a file, unmounting a save or switching app0 cannot lose dirty
// data. A barrier flushes tracked writable guest stdio, then each retained filesystem. This
// includes unrelated host files on those filesystems; it is NOT directory-level isolation.
// Windows keeps its existing process-stdio fallback; the note functions are no-ops there.
void guest_sync_note_fd(int fd);
void guest_sync_note_root(const std::string& root);
void guest_sync_note_path(const std::string& path);
void guest_sync_note_stream(FILE* stream, bool writable);
int guest_sync_close_stream(FILE* stream);
// freopen(3) and fdopen(3) for the guest. Like close, a reopen is serialized with the barrier's
// flush, and the stream it returns is tracked when `writable`. fdopen refuses a private retention
// descriptor with EBADF, as guest_sync_close_fd does.
FILE* guest_sync_reopen_stream(const char* path, const char* mode, FILE* stream, bool writable);
FILE* guest_sync_fdopen(int fd, const char* mode, bool writable);
// Private retention descriptors are not guest handles. Reject closes of those slots and
// relocate an anchor before dup2 replaces its slot, preserving the guest's requested number.
int guest_sync_close_fd(int fd);
int guest_sync_dup2(int source, int destination);
void guest_sync();

} // namespace prosper
