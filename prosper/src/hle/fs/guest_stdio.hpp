// guest_stdio.hpp — the C stdio OPEN family, for titles that ship no libc.prx.
//
// guest_stdio.cpp owns the open entry points. hle_file.cpp owns the guest mount table and the
// per-host descriptor model, and lends this file the few answers only it can give; they are
// declared here and defined there. Split out of hle_file.cpp (#4348), which is at its line cap.
#pragma once
#include <string>

namespace prosper {

// Registers the open family; called by register_file_hle(), so a test that registers only the file
// layer still reaches fopen.
void register_guest_stdio_hle();

// --- defined in hle_file.cpp ------------------------------------------------------------------

// Whether PROSPER_FILELOG is set (read once, by hle_file.cpp).
bool guest_file_log_enabled();
// The host path a guest stdio open of `guest` may use: 0 with `host` set, or the errno that refuses
// it — ENODEV for a served device, the guest namespace's errno for a path no mount owns. `create`
// says whether the open would create the file (EROFS rather than ENOENT at the guest root).
int guest_stream_host_path(const char* guest, bool create, std::string& host);

}   // namespace prosper
