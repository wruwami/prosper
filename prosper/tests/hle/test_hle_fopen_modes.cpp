// test_hle_fopen_modes — the guest's fopen, through prosper's REGISTERED stdio handlers.
//
// Contract: C11 (N1570) §7.21.5.3 (fopen modes: truncate/create, append forcing writes to the end
// regardless of fseek, update streams needing a positioning call between output and input,
// exclusive "wx"), and for everything the standard leaves open, the guest libc's own mode parser:
// Dinkumware `_Foprep`, read from the disassembly of PPSA24651's shipped libc.prx (+0x562f0) and
// named by the PS5 3.20 libSceLibcInternal exports. The grammar and its offsets are in
// guest_fopen_mode.hpp. Derived from those; no other suite's code or vectors.
//
// Scope: a title that ships libc.prx (every local dump does) binds `fopen` to its own copy, so these
// handlers serve only titles without one.
//
// What is prosper's here: `fopen` translates the guest path (/app0 → the title root) and the guest's
// mode string, and every later operation is a registered handler too (`fread`, `fwrite`, `fseek`,
// `ftell`, `fgetc`, `fflush`, `fclose`, and `__error` for the guest's errno). So each case opens a
// GUEST path through the handler and drives the stream through the handlers; the host's own stdio
// appears only to build a fixture or to observe the bytes that reached the disk.
//
// The rest of the open family — freopen, fdopen, fopen_s, freopen_s — shares fopen's mode parser
// (#4348). Each was the return-0 stub: a NULL FILE* from freopen/fdopen, and "success" with the
// out-pointer never written from fopen_s/freopen_s, which a title would read as a real stream. Their
// contracts are C11 §7.21.5.4 (freopen), POSIX fdopen, and Annex K §K.3.5.2.1-2.
#include "hle/dispatch/dispatch.hpp"
#include "hle/dispatch/nid.hpp"
#include "hle/fs/guest_fopen_mode.hpp"
#include "fixtures/test_scratch.h"
#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>

using namespace prosper;

namespace {

uint64_t ptr(const void* p) {
    return (uint64_t)(uintptr_t)p;
}

uint64_t call(const char* name, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0) {
    HleFn fn = Hle::lookup(nid_hash(name));
    if (!fn) {
        ADD_FAILURE() << name << " is not registered";
        return 0;
    }
    return fn(a, b, c, d, 0, 0);
}

class FopenModes : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        register_builtin_hle();
        root_ = (prosper_test::test_scratch_dir() / "fopen-app0").string();
        std::filesystem::create_directories(root_);
        set_app0_root(root_);
    }

    static std::string host(const std::string& name) { return root_ + "/" + name; }
    static std::string guest(const std::string& name) { return "/app0/" + name; }

    static void put(const std::string& name, const std::string& bytes) {
        std::ofstream out(host(name), std::ios::binary | std::ios::trunc);
        out.write(bytes.data(), (std::streamsize)bytes.size());
    }
    static std::string get(const std::string& name) {
        std::ifstream in(host(name), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    static bool exists(const std::string& name) { return std::filesystem::exists(host(name)); }
    static void remove(const std::string& name) { std::filesystem::remove(host(name)); }

    static FILE* open(const std::string& name, const char* mode) {
        const std::string path = guest(name);
        return (FILE*)(uintptr_t)call("fopen", ptr(path.c_str()), ptr(mode));
    }
    static int guest_errno() {
        const int* slot = (const int*)(uintptr_t)call("__error");
        return slot ? *slot : -1;
    }
    static void set_guest_errno(int value) {
        int* slot = (int*)(uintptr_t)call("__error");
        if (slot) *slot = value;
    }
    static size_t write(FILE* f, const std::string& bytes) {
        return (size_t)call("fwrite", ptr(bytes.data()), 1, bytes.size(), ptr(f));
    }
    static std::string read(FILE* f, size_t n) {
        std::string out(n, '\0');
        out.resize((size_t)call("fread", ptr(out.data()), 1, n, ptr(f)));
        return out;
    }
    static int seek(FILE* f, int64_t offset, int whence) {
        return (int)(int64_t)call("fseek", ptr(f), (uint64_t)offset, (uint64_t)whence);
    }
    static int64_t tell(FILE* f) { return (int64_t)call("ftell", ptr(f)); }
    static int getc(FILE* f) { return (int)(int64_t)call("fgetc", ptr(f)); }
    static int flush(FILE* f) { return (int)(int64_t)call("fflush", ptr(f)); }
    static int close(FILE* f) { return (int)(int64_t)call("fclose", ptr(f)); }

    // FreeBSD open(2) access modes, as the guest passes them.
    static constexpr uint64_t kGuestORdOnly = 0x0;
    static constexpr uint64_t kGuestORdWr = 0x2;
    static int guest_open(const std::string& name, uint64_t flags) {
        const std::string path = guest(name);
        return (int)(int64_t)call("open", ptr(path.c_str()), flags, 0);
    }
    static FILE* from_fd(int fd, const char* mode) {
        return (FILE*)(uintptr_t)call("fdopen", (uint64_t)(int64_t)fd, ptr(mode));
    }
    static FILE* reopen(const char* path, const char* mode, FILE* f) {
        return (FILE*)(uintptr_t)call("freopen", ptr(path), ptr(mode), ptr(f));
    }

    static inline std::string root_;
};

}   // namespace

// "r" opens an existing file at its start and fails on a missing one; "r+" never creates either.
// The path is a GUEST path: it resolves only because the handler maps /app0 to the title root.
TEST_F(FopenModes, ReadOpensExistingAndFailsOnMissing) {
    put("read.bin", "hello");
    FILE* f = open("read.bin", "r");
    ASSERT_NE(f, nullptr) << "a guest /app0 path opens through the handler";
    EXPECT_EQ(read(f, 16), "hello");
    EXPECT_EQ(getc(f), EOF);
    EXPECT_NE(call("feof", ptr(f)), 0u);
    EXPECT_EQ(close(f), 0);

    remove("absent.bin");
    for (const char* mode : {"r", "rb", "r+", "r+b"}) {
        set_guest_errno(0);
        EXPECT_EQ(open("absent.bin", mode), nullptr) << mode;
        EXPECT_EQ(guest_errno(), ENOENT) << mode;
        EXPECT_FALSE(exists("absent.bin")) << mode << " must not create";
    }
}

// "w" truncates at OPEN time, before anything is written, and creates a missing file.
TEST_F(FopenModes, WriteTruncatesOrCreates) {
    put("trunc.bin", "0123456789");
    FILE* f = open("trunc.bin", "w");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(std::filesystem::file_size(host("trunc.bin")), 0u) << "truncated by the open itself";
    EXPECT_EQ(write(f, "ab"), 2u);
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("trunc.bin"), "ab");

    remove("created.bin");
    f = open("created.bin", "wb");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(write(f, "new"), 3u);
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("created.bin"), "new");
}

// Update streams: "r+" keeps the contents and overwrites in place; "w+" truncates and can read
// back what it wrote. Input after output needs an intervening positioning call (§7.21.5.3p7).
TEST_F(FopenModes, UpdateModesReadAndWrite) {
    put("rplus.bin", "0123456789");
    FILE* f = open("rplus.bin", "r+");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(write(f, "AB"), 2u);
    EXPECT_EQ(seek(f, 0, SEEK_SET), 0);
    EXPECT_EQ(read(f, 4), "AB23");
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("rplus.bin"), "AB23456789") << "r+ neither truncates nor appends";

    put("wplus.bin", "old contents");
    f = open("wplus.bin", "w+");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(write(f, "xyz"), 3u);
    EXPECT_EQ(seek(f, 0, SEEK_SET), 0);
    EXPECT_EQ(read(f, 16), "xyz");
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("wplus.bin"), "xyz");
}

// Append forces every write to the then-current end of file regardless of fseek (§7.21.5.3p6). The
// stream is positioned AWAY from the end before each write, so an ordinary positioned write would
// land elsewhere. Where "a+" first reads from is not specified, so it seeks before reading.
TEST_F(FopenModes, AppendForcesWritesToTheEnd) {
    put("append.bin", "base");
    FILE* f = open("append.bin", "a");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(seek(f, 0, SEEK_SET), 0);
    EXPECT_EQ(write(f, "X"), 1u);
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("append.bin"), "baseX");

    put("aplus.bin", "base");
    f = open("aplus.bin", "a+");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(seek(f, 0, SEEK_SET), 0);
    EXPECT_EQ(getc(f), 'b') << "a+ reads from wherever it is positioned";
    EXPECT_EQ(seek(f, 1, SEEK_SET), 0);
    EXPECT_EQ(write(f, "Y"), 1u);
    EXPECT_EQ(flush(f), 0);
    EXPECT_EQ(tell(f), 5) << "after an append write the stream is at the new end";
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("aplus.bin"), "baseY");

    remove("acreate.bin");
    f = open("acreate.bin", "a");
    ASSERT_NE(f, nullptr) << "append creates a missing file";
    EXPECT_EQ(write(f, "z"), 1u);
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("acreate.bin"), "z");
}

// "wx" fails if the file exists, leaving it untouched, and creates it otherwise (§7.21.5.3p5).
TEST_F(FopenModes, ExclusiveCreate) {
    put("excl.bin", "keep");
    for (const char* mode : {"wx", "wbx", "w+x", "w+bx"}) {
        set_guest_errno(0);
        EXPECT_EQ(open("excl.bin", mode), nullptr) << mode;
        EXPECT_EQ(guest_errno(), EEXIST) << mode;
        EXPECT_EQ(get("excl.bin"), "keep") << mode << " must not truncate an existing file";
    }
    remove("excl-new.bin");
    FILE* f = open("excl-new.bin", "wx");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(write(f, "fresh"), 5u);
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("excl-new.bin"), "fresh");
}

// A mode that does not begin with 'r', 'w' or 'a' is EINVAL and opens nothing (`_Foprep` stores
// EINVAL at libc.prx+0x5643a and returns NULL).
TEST_F(FopenModes, InvalidLeadingCharacterIsEinval) {
    remove("invalid.bin");
    for (const char* mode : {"", "z", "+r", "br", "x", "W"}) {
        set_guest_errno(0);
        EXPECT_EQ(open("invalid.bin", mode), nullptr) << "'" << mode << "'";
        EXPECT_EQ(guest_errno(), EINVAL) << "'" << mode << "'";
        EXPECT_FALSE(exists("invalid.bin")) << "'" << mode << "' must not create";
    }
}

// --- the guest's mode grammar, which the host's is not (guest_fopen_mode.hpp) -------------------

// A null mode is EINVAL. This is prosper's own lenient choice, not the guest's contract: `_Foprep`
// reads mode[0] with no null check (libc.prx+0x563a5), so on hardware it would fault.
TEST_F(FopenModes, NullModeIsEinval) {
    put("nullmode.bin", "x");
    const std::string path = guest("nullmode.bin");
    set_guest_errno(0);
    EXPECT_EQ(call("fopen", ptr(path.c_str()), 0), 0u);
    EXPECT_EQ(guest_errno(), EINVAL);
}

// The guest has no text mode: `_Foprep`'s 'b' sets a bit `_Fopen` never maps to an open flag, so
// "r" and "w" are byte-exact whether or not 'b' is given. A Windows host
// that opened them in text mode would collapse CR LF to LF on read, stop at 0x1A, and expand LF to
// CR LF on write.
TEST_F(FopenModes, NoTextModeTranslation) {
    const std::string bytes("a\r\nb\x1a"
                            "c\n\0d\r",
                            9);
    put("binary.bin", bytes);
    for (const char* mode : {"r", "rb", "r+"}) {
        FILE* f = open("binary.bin", mode);
        ASSERT_NE(f, nullptr) << mode;
        EXPECT_EQ(read(f, 64), bytes) << mode << " must read every byte unchanged";
        EXPECT_EQ(close(f), 0);
    }
    for (const char* mode : {"w", "w+", "a"}) {
        remove("written.bin");
        FILE* f = open("written.bin", mode);
        ASSERT_NE(f, nullptr) << mode;
        EXPECT_EQ(write(f, bytes), bytes.size()) << mode;
        EXPECT_EQ(close(f), 0);
        EXPECT_EQ(get("written.bin"), bytes) << mode << " must write every byte unchanged";
    }
}

// 'x' on a read-only stream is NOT refused: `_Foprep` never rejects a mode for its 'x', and on 'r'
// the guest's open carries O_EXCL without O_CREAT, which does not refuse an existing file. So "rx"
// opens the existing file read-only, exactly like "r".
TEST_F(FopenModes, ExclusiveOnAReadOnlyStreamOpensReadOnly) {
    put("rx.bin", "data");
    set_guest_errno(0);
    FILE* f = open("rx.bin", "rx");
    ASSERT_NE(f, nullptr) << "the guest's libc opens \"rx\"";
    EXPECT_EQ(read(f, 16), "data");
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("rx.bin"), "data");
}

// Only the character that ENDS `_Foprep`'s loop can be 'x', and '+' and 'b' are each read once:
//   "rx+"  — 'x' ends the mode before '+': read-only (a write fails), file untouched.
//   "rbb+" — the second 'b' ends it: read-only.
//   "wx+"  — 'x' ends it: write-only and exclusive (refuses an existing file; reading fails).
//   "we+"  — 'e' is not a guest letter and ends it: plain write-only "w".
//   "w++x" — the second '+' ends it, so the 'x' is never read: "w+", which truncates.
TEST_F(FopenModes, OnlyTheTerminatingLetterCanBeExclusive) {
    for (const char* mode : {"rx+", "rbb+"}) {
        put("ro.bin", "data");
        FILE* f = open("ro.bin", mode);
        ASSERT_NE(f, nullptr) << mode;
        EXPECT_EQ(write(f, "ZZ"), 0u) << mode << " is read-only in the guest";
        EXPECT_EQ(close(f), 0);
        EXPECT_EQ(get("ro.bin"), "data") << mode;
    }

    put("wxplus.bin", "keep");
    set_guest_errno(0);
    EXPECT_EQ(open("wxplus.bin", "wx+"), nullptr) << "\"wx+\" is exclusive";
    EXPECT_EQ(guest_errno(), EEXIST);
    EXPECT_EQ(get("wxplus.bin"), "keep");

    for (const char* mode : {"wx+", "we+"}) {
        remove("wo.bin");
        FILE* f = open("wo.bin", mode);
        ASSERT_NE(f, nullptr) << mode;
        EXPECT_EQ(write(f, "abc"), 3u) << mode;
        EXPECT_EQ(seek(f, 0, SEEK_SET), 0) << mode;
        EXPECT_EQ(read(f, 16), "") << mode << " is write-only in the guest";
        EXPECT_EQ(close(f), 0);
        EXPECT_EQ(get("wo.bin"), "abc") << mode;
    }

    put("wpp.bin", "existing");
    FILE* f = open("wpp.bin", "w++x");
    ASSERT_NE(f, nullptr) << "the 'x' after a second '+' is not part of the mode";
    EXPECT_EQ(write(f, "q"), 1u);
    EXPECT_EQ(seek(f, 0, SEEK_SET), 0);
    EXPECT_EQ(read(f, 16), "q") << "\"w++x\" is the update stream \"w+\"";
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("wpp.bin"), "q");
}

// An unrecognised letter ENDS the guest's mode: what follows it is not read (libc.prx+0x56462). So
// "wqx" is plain "w" — it truncates an existing file instead of failing — where a host that skips
// unknown letters would honour the 'x' and refuse.
TEST_F(FopenModes, UnknownLetterEndsTheMode) {
    put("unknown.bin", "existing");
    FILE* f = open("unknown.bin", "wqx");
    ASSERT_NE(f, nullptr) << "the 'x' after an unknown letter is not part of the mode";
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("unknown.bin"), "") << "opened as plain \"w\", which truncates";
}

// "ax" fails on an existing file where the host can spell it; on Windows, whose CRT cannot, it is
// refused with EINVAL rather than opened without the exclusivity the guest asked for.
TEST_F(FopenModes, AppendExclusive) {
    put("ax.bin", "keep");
    set_guest_errno(0);
    EXPECT_EQ(open("ax.bin", "ax"), nullptr);
#if defined(_WIN32)
    EXPECT_EQ(guest_errno(), EINVAL);
#else
    EXPECT_EQ(guest_errno(), EEXIST);
#endif
    EXPECT_EQ(get("ax.bin"), "keep");
}

// --- the rest of the open family: freopen, fdopen, fopen_s, freopen_s (#4348) -------------------
// The errno values these return (fopen_s, freopen_s) are FreeBSD numbers; every one asserted here —
// ENOENT 2, EBADF 9, EEXIST 17, EINVAL 22 — has the same number on every host.

// freopen points the SAME stream at another guest path, under the new mode.
TEST_F(FopenModes, FreopenRedirectsTheSameStream) {
    put("first.bin", "first");
    put("second.bin", "second");
    FILE* f = open("first.bin", "r");
    ASSERT_NE(f, nullptr);
    const std::string second = guest("second.bin");
    ASSERT_EQ(reopen(second.c_str(), "r", f), f) << "freopen returns the stream it was given";
    EXPECT_EQ(read(f, 16), "second");

    const std::string first = guest("first.bin");
    ASSERT_EQ(reopen(first.c_str(), "w", f), f);
    EXPECT_EQ(std::filesystem::file_size(host("first.bin")), 0u) << "\"w\" truncates at the reopen";
    EXPECT_EQ(write(f, "new"), 3u);
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("first.bin"), "new");
    EXPECT_EQ(get("second.bin"), "second");
}

// A refused reopen has still closed the stream's file first (C11 §7.21.5.4p4), so what the stream
// had buffered reaches the old file. One case per refusal: the mode, a path outside every mount,
// and the host's own open.
TEST_F(FopenModes, FreopenFailureClosesTheOldFile) {
    struct Case {
        const char* path;
        const char* mode;
        int error;
    };
    remove("absent-reopen.bin");
    const std::string absent = guest("absent-reopen.bin");
    const Case cases[] = {
        {absent.c_str(), "z", EINVAL},
        {"/nowhere/at/all.bin", "r", ENOENT},
        {absent.c_str(), "r", ENOENT},
    };
    for (const Case& c : cases) {
        FILE* f = open("pending.bin", "w");
        ASSERT_NE(f, nullptr);
        EXPECT_EQ(write(f, "pending"), 7u);
        set_guest_errno(0);
        EXPECT_EQ(reopen(c.path, c.mode, f), nullptr) << c.path << " '" << c.mode << "'";
        EXPECT_EQ(guest_errno(), c.error) << c.path << " '" << c.mode << "'";
        EXPECT_EQ(get("pending.bin"), "pending")
            << c.path << " '" << c.mode << "': the old file was flushed and closed";
        EXPECT_FALSE(exists("absent-reopen.bin"));
    }
}

// A null path asks for a mode change in place, which prosper does not support: it is refused
// EBADF and, unlike every other refusal, the stream stays open and usable. A null stream is EINVAL.
TEST_F(FopenModes, FreopenNullPathOrStreamIsRefused) {
    put("keep-open.bin", "still here");
    FILE* f = open("keep-open.bin", "r");
    ASSERT_NE(f, nullptr);
    set_guest_errno(0);
    EXPECT_EQ(reopen(nullptr, "rb", f), nullptr);
    EXPECT_EQ(guest_errno(), EBADF);
    EXPECT_EQ(read(f, 32), "still here") << "the refused stream is still open";
    EXPECT_EQ(close(f), 0);

    const std::string path = guest("keep-open.bin");
    set_guest_errno(0);
    EXPECT_EQ(reopen(path.c_str(), "r", nullptr), nullptr);
    EXPECT_EQ(guest_errno(), EINVAL);
}

// fdopen stands a stream on a descriptor the guest's own open returned, and fclose closes both.
// "w" neither truncates nor creates — the file is already open — and an 'x' asks nothing of a
// descriptor that already exists.
TEST_F(FopenModes, FdopenStandsAStreamOnAGuestDescriptor) {
    put("fd.bin", "0123456789");
    int fd = guest_open("fd.bin", kGuestORdWr);
    ASSERT_GE(fd, 3);
    FILE* f = from_fd(fd, "r+");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(read(f, 4), "0123");
    EXPECT_EQ(seek(f, 0, SEEK_SET), 0);
    EXPECT_EQ(write(f, "AB"), 2u);
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("fd.bin"), "AB23456789");
    EXPECT_EQ((int64_t)call("close", (uint64_t)fd), -1) << "fclose closed the descriptor too";

    fd = guest_open("fd.bin", kGuestORdWr);
    ASSERT_GE(fd, 3);
    f = from_fd(fd, "w");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(std::filesystem::file_size(host("fd.bin")), 10u) << "fdopen \"w\" does not truncate";
    EXPECT_EQ(write(f, "Z"), 1u);
    EXPECT_EQ(close(f), 0);
    EXPECT_EQ(get("fd.bin"), "ZB23456789");

    fd = guest_open("fd.bin", kGuestORdWr);
    ASSERT_GE(fd, 3);
    f = from_fd(fd, "w+x");
    ASSERT_NE(f, nullptr) << "exclusivity belongs to the open that made the descriptor";
    EXPECT_EQ(read(f, 16), "ZB23456789");
    EXPECT_EQ(close(f), 0);
}

// A bad mode is EINVAL and leaves the descriptor open; a descriptor that is not open is EBADF; the
// random device is ENODEV, as it is for fopen (a host stream would read its null backing object).
TEST_F(FopenModes, FdopenRefusals) {
    put("fdbad.bin", "x");
    const int fd = guest_open("fdbad.bin", kGuestORdOnly);
    ASSERT_GE(fd, 3);
    for (const char* mode : {"z", "", (const char*)nullptr}) {
        set_guest_errno(0);
        EXPECT_EQ(from_fd(fd, mode), nullptr) << (mode ? mode : "(null)");
        EXPECT_EQ(guest_errno(), EINVAL) << (mode ? mode : "(null)");
    }
    EXPECT_EQ(call("close", (uint64_t)fd), 0u) << "a refused fdopen leaves the descriptor open";

    set_guest_errno(0);
    EXPECT_EQ(from_fd(fd, "r"), nullptr) << "the descriptor is closed now";
    EXPECT_EQ(guest_errno(), EBADF);
    set_guest_errno(0);
    EXPECT_EQ(from_fd(-1, "r"), nullptr);
    EXPECT_EQ(guest_errno(), EBADF);

    const int dev = (int)(int64_t)call("open", ptr("/dev/urandom"), kGuestORdOnly, 0);
    ASSERT_GE(dev, 3);
    set_guest_errno(0);
    EXPECT_EQ(from_fd(dev, "r"), nullptr);
    EXPECT_EQ(guest_errno(), ENODEV);
    EXPECT_EQ(call("close", (uint64_t)dev), 0u);
}

// Descriptors 0-2 are prosper's own stdio, which the guest's close leaves open. A stream fdopen'd on
// one stands on a duplicate, so its fclose leaves the descriptor open too.
TEST_F(FopenModes, FdopenOnStdioDoesNotCloseIt) {
    constexpr uint64_t kGuestFGetFd = 1;
    if ((int64_t)call("fcntl", 2, kGuestFGetFd) < 0) GTEST_SKIP() << "no stderr descriptor";
    FILE* f = from_fd(2, "w");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(close(f), 0);
    EXPECT_GE((int64_t)call("fcntl", 2, kGuestFGetFd), 0) << "fd 2 survives its stream's fclose";
}

#if defined(_WIN32)
// On Windows a directory descriptor is prosper's own snapshot, not a CRT descriptor: no host stream
// can stand on it, so fdopen refuses it rather than hand the CRT a number it does not own.
TEST_F(FopenModes, FdopenRefusesADirectoryDescriptor) {
    std::filesystem::create_directories(host("fd-dir"));
    const int fd = guest_open("fd-dir", kGuestORdOnly);
    ASSERT_GE(fd, 3);
    set_guest_errno(0);
    EXPECT_EQ(from_fd(fd, "r"), nullptr);
    EXPECT_EQ(guest_errno(), EISDIR);
    EXPECT_EQ(call("close", (uint64_t)fd), 0u);
}
#endif

// fopen_s: zero, with the stream stored through the out-pointer; or an errno value, with NULL
// stored there. A null out-pointer, path or mode is EINVAL.
TEST_F(FopenModes, FopenSReturnsAnErrnoAndWritesTheOutPointer) {
    static int sentinel;
    FILE* const unset = (FILE*)&sentinel;
    put("fopens.bin", "annex k");
    const std::string path = guest("fopens.bin");
    FILE* out = unset;
    EXPECT_EQ(call("fopen_s", ptr(&out), ptr(path.c_str()), ptr("r")), 0u);
    ASSERT_NE(out, nullptr);
    ASSERT_NE(out, unset) << "the stream was stored through the out-pointer";
    EXPECT_EQ(read(out, 16), "annex k");
    EXPECT_EQ(close(out), 0);

    struct Case {
        const char* path;
        const char* mode;
        uint64_t status;
    };
    remove("fopens-absent.bin");
    const std::string absent = guest("fopens-absent.bin");
    const Case cases[] = {
        {absent.c_str(), "r", ENOENT}, {path.c_str(), "wx", EEXIST},    {path.c_str(), "z", EINVAL},
        {nullptr, "r", EINVAL},        {path.c_str(), nullptr, EINVAL},
    };
    for (const Case& c : cases) {
        out = unset;
        EXPECT_EQ(call("fopen_s", ptr(&out), ptr(c.path), ptr(c.mode)), c.status)
            << (c.path ? c.path : "(null)") << " '" << (c.mode ? c.mode : "(null)") << "'";
        EXPECT_EQ(out, nullptr) << "a failed fopen_s stores NULL";
    }
    EXPECT_FALSE(exists("fopens-absent.bin"));
    EXPECT_EQ(get("fopens.bin"), "annex k") << "\"wx\" left the existing file alone";
    EXPECT_EQ(call("fopen_s", 0, ptr(path.c_str()), ptr("r")), (uint64_t)EINVAL)
        << "null out-pointer";
}

// freopen_s: zero with the given stream stored on success. A null out-pointer, mode or stream is
// EINVAL and closes nothing; a null path is the mode change freopen refuses (EBADF), which also
// leaves the stream open; a failed open closes the stream and returns the error, storing NULL.
TEST_F(FopenModes, FreopenSReturnsAnErrnoAndWritesTheOutPointer) {
    static int sentinel;
    FILE* const unset = (FILE*)&sentinel;
    put("reopens-a.bin", "a");
    put("reopens-b.bin", "b");
    const std::string b = guest("reopens-b.bin");
    FILE* f = open("reopens-a.bin", "r");
    ASSERT_NE(f, nullptr);
    FILE* out = unset;
    EXPECT_EQ(call("freopen_s", ptr(&out), ptr(b.c_str()), ptr("r"), ptr(f)), 0u);
    ASSERT_EQ(out, f) << "success stores the stream it was given";
    EXPECT_EQ(read(f, 4), "b");

    out = unset;
    EXPECT_EQ(call("freopen_s", ptr(&out), ptr(b.c_str()), 0, ptr(f)), (uint64_t)EINVAL)
        << "null mode";
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(call("freopen_s", 0, ptr(b.c_str()), ptr("r"), ptr(f)), (uint64_t)EINVAL)
        << "null out-pointer";
    out = unset;
    EXPECT_EQ(call("freopen_s", ptr(&out), ptr(b.c_str()), ptr("r"), 0), (uint64_t)EINVAL)
        << "null stream";
    EXPECT_EQ(out, nullptr);
    out = unset;
    EXPECT_EQ(call("freopen_s", ptr(&out), 0, ptr("r"), ptr(f)), (uint64_t)EBADF) << "null path";
    EXPECT_EQ(out, nullptr);
    EXPECT_EQ(seek(f, 0, SEEK_SET), 0);
    EXPECT_EQ(read(f, 4), "b") << "none of those refusals closed the stream";

    remove("reopens-absent.bin");
    const std::string absent = guest("reopens-absent.bin");
    out = unset;
    EXPECT_EQ(call("freopen_s", ptr(&out), ptr(absent.c_str()), ptr("r"), ptr(f)),
              (uint64_t)ENOENT);
    EXPECT_EQ(out, nullptr) << "a failed freopen_s stores NULL (and has closed the stream)";
}

// The translation itself, for BOTH host dialects on every host — the Windows spelling is checked
// here on Linux too, since only the Windows CI job would otherwise execute it.
TEST(GuestFopenModeParse, SpellsEachHostDialect) {
    struct Row {
        const char* guest;
        const char* posix;   // nullptr: EINVAL
        const char* microsoft;
    };
    const Row rows[] = {
        {"r", "rb", "rb"},           {"rb", "rb", "rb"},         {"w", "wb", "wb"},
        {"a", "ab", "ab"},           {"r+", "r+b", "r+b"},       {"rb+", "r+b", "r+b"},
        {"r+b", "r+b", "r+b"},       {"w+", "w+b", "w+b"},       {"a+", "a+b", "a+b"},
        {"wx", "wbx", "wbx"},        {"w+x", "w+bx", "w+bx"},    {"wbx", "wbx", "wbx"},
        {"w+bx", "w+bx", "w+bx"},    {"wb+x", "w+bx", "w+bx"},   {"a+x", "a+bx", nullptr},
        {"ab+x", "a+bx", nullptr},   {"ax", "abx", nullptr},     {"r+x", "r+b", "r+b"},
        {"rb+x", "r+b", "r+b"},      {"rx", "rb", "rb"},         {"rx+", "rb", "rb"},
        {"rxb", "rb", "rb"},         {"wx+", "wbx", "wbx"},      {"rbb+", "rb", "rb"},
        {"r++", "r+b", "r+b"},       {"r+b+", "r+b", "r+b"},     {"w++x", "w+b", "w+b"},
        {"wbbx", "wb", "wb"},        {"r+bb", "r+b", "r+b"},     {"re", "rb", "rb"},
        {"we+", "wb", "wb"},         {"w+xe", "w+bx", "w+bx"},   {"ae", "ab", "ab"},
        {"rt", "rb", "rb"},          {"rt+", "rb", "rb"},        {"wqx", "wb", "wb"},
        {"r,ccs=UTF-8", "rb", "rb"}, {"", nullptr, nullptr},     {"z", nullptr, nullptr},
        {"+r", nullptr, nullptr},    {"b", nullptr, nullptr},    {"R", nullptr, nullptr},
        {"x", nullptr, nullptr},     {"xw", nullptr, nullptr},
    };
    for (const Row& row : rows) {
        const GuestFopenMode p = parse_guest_fopen_mode(row.guest, FopenHostDialect::Posix);
        const GuestFopenMode m = parse_guest_fopen_mode(row.guest, FopenHostDialect::MicrosoftCrt);
        EXPECT_EQ(p.valid, row.posix != nullptr) << "'" << row.guest << "'";
        EXPECT_EQ(m.valid, row.microsoft != nullptr) << "'" << row.guest << "'";
        if (row.posix) EXPECT_STREQ(p.host, row.posix) << "'" << row.guest << "'";
        if (row.microsoft) EXPECT_STREQ(m.host, row.microsoft) << "'" << row.guest << "'";
    }
    EXPECT_FALSE(parse_guest_fopen_mode(nullptr, FopenHostDialect::Posix).valid);

    const GuestFopenMode r = parse_guest_fopen_mode("r", FopenHostDialect::Posix);
    EXPECT_TRUE(r.readable && !r.writable && !r.append);
    const GuestFopenMode a = parse_guest_fopen_mode("a+", FopenHostDialect::Posix);
    EXPECT_TRUE(a.readable && a.writable && a.append);
    const GuestFopenMode w = parse_guest_fopen_mode("wbx", FopenHostDialect::Posix);
    EXPECT_TRUE(!w.readable && w.writable && w.create && w.exclusive);
    // "rx" carries the guest's exclusive flag (O_EXCL without O_CREAT) but no host 'x'.
    const GuestFopenMode rx = parse_guest_fopen_mode("rx", FopenHostDialect::Posix);
    EXPECT_TRUE(rx.readable && !rx.writable && !rx.create && rx.exclusive);
    // A second '+' or 'b' ends the loop, so a later 'x' does not count.
    EXPECT_FALSE(parse_guest_fopen_mode("w++x", FopenHostDialect::Posix).exclusive);
    EXPECT_FALSE(parse_guest_fopen_mode("wbbx", FopenHostDialect::Posix).exclusive);
}
