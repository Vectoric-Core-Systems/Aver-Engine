// aver::File -- ranged, non-truncating, durable file access. Exit code = failure count.
//
// Real files on the real filesystem, like WatcherTest and for the same reason: every property worth
// checking here is the operating system's behaviour rather than the class's. A fake backend would
// return whatever it was told to and prove nothing about whether a write at a 4 GiB offset lands where
// it was asked, which is exactly the sort of thing that is wrong until measured.
//
// WHAT THIS GUARDS. The region format (docs/CHUNKS.md section 5) rests on three properties of this
// type, and each is a silent corruption if it does not hold:
//   * writeAt does not truncate -- otherwise patching one directory entry destroys the archive.
//   * readAt is EXACT -- a short read of a directory entry is indistinguishable from a valid entry
//     pointing somewhere arbitrary.
//   * 64-bit offsets work -- a region archive can exceed 4 GiB, and truncating the offset to 32 bits
//     writes over the start of the file rather than failing.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#include <winioctl.h>
#endif

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;
static int g_skipped = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

int main() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "aver-filerange-test";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string pathA = (dir / "a.bin").string();
    const std::string pathB = (dir / "b.bin").string();

    // ---- open semantics --------------------------------------------------------------------------
    {
        File f;
        check(!f.isOpen(), "a default-constructed File is closed");
        check(f.size() == -1, "...and reports size -1 rather than 0");
        check(!f.readAt(0, nullptr, 0) || true, "reading a closed file does not crash");
        check(!f.open(pathA, File::Mode::Read), "Read mode refuses a file that does not exist");
        check(!f.open(pathA, File::Mode::ReadWrite), "ReadWrite mode refuses one too");
        check(f.open(pathA, File::Mode::Create), "Create mode makes it");
        check(f.isOpen() && f.size() == 0, "the new file is open and empty");
    }

    // ---- write, read back, and the non-truncating promise -----------------------------------------
    {
        File f;
        check(f.open(pathA, File::Mode::ReadWrite), "an existing file opens ReadWrite");

        std::vector<u8> payload(4096);
        for (usize i = 0; i < payload.size(); ++i) payload[i] = static_cast<u8>(i * 7 + 3);
        check(f.writeAt(0, payload.data(), payload.size()), "a 4 KiB write at offset 0 succeeds");
        check(f.size() == 4096, "the file is now exactly 4096 bytes");

        std::vector<u8> got(4096);
        check(f.readAt(0, got.data(), got.size()), "it reads back");
        check(std::memcmp(got.data(), payload.data(), payload.size()) == 0, "byte for byte");

        // A ranged read of the middle -- the operation the whole type exists for.
        std::vector<u8> mid(100);
        check(f.readAt(1000, mid.data(), mid.size()), "a 100-byte read at offset 1000 succeeds");
        check(std::memcmp(mid.data(), payload.data() + 1000, 100) == 0,
              "...and returns exactly those bytes, not the start of the file");

        // THE PROMISE THAT SEPARATES THIS FROM writeFileBytes.
        const u8 patch[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        check(f.writeAt(64, patch, sizeof patch), "a 4-byte patch at offset 64 succeeds");
        check(f.size() == 4096, "and the file is STILL 4096 bytes -- writeAt never truncates");
        u8 back[4]{};
        check(f.readAt(64, back, sizeof back), "the patch reads back");
        check(std::memcmp(back, patch, 4) == 0, "...as what was written");
        u8 neighbour[4]{};
        check(f.readAt(68, neighbour, sizeof neighbour), "the bytes after it read back");
        check(std::memcmp(neighbour, payload.data() + 68, 4) == 0, "...unchanged");

        check(f.sync(), "sync() succeeds on a writable handle");
    }

    // ---- reads that must FAIL rather than half-succeed ---------------------------------------------
    {
        File f;
        check(f.open(pathA, File::Mode::Read), "reopening read-only works");
        std::vector<u8> buf(64);
        check(f.readAt(4096 - 64, buf.data(), 64), "a read ending exactly at EOF succeeds");
        check(!f.readAt(4096 - 63, buf.data(), 64), "a read running one byte PAST EOF fails");
        check(!f.readAt(4096, buf.data(), 1), "a read starting at EOF fails");
        check(!f.readAt(1'000'000, buf.data(), 1), "a read far past EOF fails");
        check(f.readAt(0, buf.data(), 0), "a zero-byte read is a no-op success");

        // A read-only handle must reject writes rather than silently doing nothing.
        const u8 one = 1;
        check(!f.writeAt(0, &one, 1), "a Read-mode handle refuses writeAt");
        check(!f.setSize(10), "...and refuses setSize");
    }

    // ---- writing past the end, and setSize ----------------------------------------------------------
    {
        File f;
        check(f.open(pathB, File::Mode::Create), "a second file is created");
        const u8 marker[4] = {1, 2, 3, 4};
        check(f.writeAt(8192, marker, sizeof marker), "a write far past EOF succeeds");
        check(f.size() == 8196, "...and grows the file to cover it");
        u8 gap[8]{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        check(f.readAt(4000, gap, sizeof gap), "the gap it left is readable");
        bool zeroed = true;
        for (const u8 b : gap) if (b != 0) zeroed = false;
        check(zeroed, "...and reads back as zeros, not as stale disk contents");

        check(f.setSize(100), "setSize can shrink");
        check(f.size() == 100, "...and the file really is 100 bytes");
        check(f.setSize(200), "setSize can grow");
        check(f.size() == 200, "...to exactly 200");
        u8 tail[8]{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        check(f.readAt(150, tail, sizeof tail), "the grown region is readable");
        zeroed = true;
        for (const u8 b : tail) if (b != 0) zeroed = false;
        check(zeroed, "...and is zero-filled");
    }

    // ---- the crash-detection property ---------------------------------------------------------------
    //
    // The region format's recovery argument is that a torn write is DETECTABLE rather than silently
    // plausible. At this layer that reduces to: truncate a file in the middle of a record, and the
    // read of that record must fail outright instead of returning a short buffer the caller might
    // mistake for a whole one. Slice 5 layers a CRC and a doubled header on top of this; neither
    // helps if the read itself lies.
    {
        const std::string pathC = (dir / "torn.bin").string();
        {
            File f;
            check(f.open(pathC, File::Mode::Create), "a file for the torn-write case");
            std::vector<u8> rec(512, 0xA5);
            check(f.writeAt(0, rec.data(), rec.size()), "a 512-byte record is written whole");
            check(f.sync(), "and synced");
        }
        {
            File f;
            check(f.open(pathC, File::Mode::ReadWrite), "reopened");
            check(f.setSize(300), "the file is cut mid-record, as a crash would");
        }
        {
            File f;
            check(f.open(pathC, File::Mode::Read), "reopened read-only");
            std::vector<u8> rec(512);
            check(!f.readAt(0, rec.data(), 512),
                  "reading the whole record FAILS -- a torn write is detectable, not plausible");
            check(f.readAt(0, rec.data(), 300), "the surviving prefix still reads, so recovery is possible");
        }
    }

    // ---- 64-bit offsets ------------------------------------------------------------------------------
    //
    // A region archive can exceed 4 GiB, and an offset truncated to 32 bits does not fail -- it
    // writes over the START of the file. That is the single worst failure this type could have, so it
    // is worth the trouble of a sparse file to check: setSize alone would reserve real disk space,
    // which is not something a test should do to somebody's drive.
    {
        const std::string pathD = (dir / "sparse.bin").string();
        bool sparse = false;
#if defined(_WIN32)
        {
            HANDLE h = CreateFileW(std::filesystem::path(pathD).wstring().c_str(),
                                   GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD n = 0;
                sparse = DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &n, nullptr) != 0;
                CloseHandle(h);
            }
        }
#endif
        if (!sparse) {
            AVER_WARN("   SKIP  the >4 GiB offset check needs a sparse file, which this filesystem "
                      "declined -- the 64-bit path is unverified here");
            ++g_skipped;
        } else {
            const u64 farOff = (1ull << 32) + 4096;   // 4 GiB + 4 KiB, comfortably past a u32
            File f;
            check(f.open(pathD, File::Mode::ReadWrite), "the sparse file opens");
            const u8 mark[8] = {'F', 'A', 'R', 'B', 'Y', 'T', 'E', 'S'};
            check(f.writeAt(farOff, mark, sizeof mark), "an 8-byte write at 4 GiB + 4 KiB succeeds");
            check(f.size() == static_cast<i64>(farOff + 8), "the file's size reflects the far offset");
            u8 got[8]{};
            check(f.readAt(farOff, got, sizeof got), "it reads back from the same far offset");
            check(std::memcmp(got, mark, 8) == 0, "...as what was written");
            // The actual bug being guarded: a truncated offset writes at (farOff & 0xFFFFFFFF) == 4096.
            u8 low[8]{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            check(f.readAt(4096, low, sizeof low), "offset 4096 -- where a 32-bit truncation would land -- reads");
            bool untouched = true;
            for (const u8 b : low) if (b != 0) untouched = false;
            check(untouched, "...and is still zero: the offset was NOT truncated to 32 bits");
        }
    }

    // ---- move semantics --------------------------------------------------------------------------
    {
        File a;
        check(a.open(pathA, File::Mode::Read), "a file to move from");
        File b = std::move(a);
        check(b.isOpen(), "the moved-to File owns the handle");
        check(!a.isOpen(), "and the moved-from File is closed, not double-closing at scope exit");
        u8 one = 0;
        check(b.readAt(0, &one, 1), "the moved handle still reads");
    }

    // ---- delete and rename -----------------------------------------------------------------------
    {
        const std::string from = (dir / "from.tmp").string();
        const std::string to   = (dir / "to.bin").string();
        {
            File f;
            check(f.open(from, File::Mode::Create), "a temp file to swap in");
            const u8 v[4] = {9, 9, 9, 9};
            check(f.writeAt(0, v, 4), "with contents");
        }
        {
            File f;
            check(f.open(to, File::Mode::Create), "an existing destination");
            const u8 v[4] = {1, 1, 1, 1};
            check(f.writeAt(0, v, 4), "with different contents");
        }
        check(renameFile(from, to), "rename REPLACES the existing destination");
        check(!std::filesystem::exists(from, ec), "the source is gone");
        {
            File f;
            check(f.open(to, File::Mode::Read), "the destination opens");
            u8 v[4]{};
            check(f.readAt(0, v, 4), "and reads");
            check(v[0] == 9 && v[3] == 9, "...as the SOURCE's contents -- the swap really happened");
        }
        check(deleteFile(to), "deleteFile removes a file that exists");
        check(!std::filesystem::exists(to, ec), "...and it is gone");
        check(deleteFile(to), "deleteFile on a file that does not exist is SUCCESS, not failure");
        check(deleteFile((dir / "never-existed.bin").string()),
              "...and so is one that never existed at all");
    }

    // ---- writeFileBytesAtomic / writeFileTextAtomic: the crash-safe writer ------------------------
    //
    // writeFileBytes/writeFileText truncate the instant they open, before a single byte of new
    // content has landed -- fine for build output a process regenerates, and the exact bug this
    // release fixed for the level, material and graph writers, which used to open their destination
    // directly with ios::trunc. writeFileBytesAtomic writes to a temporary and swaps it in with
    // renameFile instead (FileSystem.cpp), so this checks both halves of that promise: a successful
    // write really lands, and a write that CANNOT complete leaves the original exactly as it was --
    // never truncated, never half-written -- with no ".tmp" left behind either way.
    {
        const std::string target = (dir / "atomic.txt").string();
        const std::string tmp = target + ".tmp";

        // A first save: the destination does not exist yet, which must be an ordinary success and
        // not a special case the caller has to know about.
        check(writeFileTextAtomic(target, "first version"), "an atomic write to a new path succeeds");
        std::string readBack;
        check(readFileText(target, readBack) && readBack == "first version",
              "...and the content on disk is exactly what was written");
        check(!std::filesystem::exists(tmp, ec), "...with no '.tmp' left behind after success");

        // An ordinary overwrite: the old content is fully replaced, not appended to or merged with.
        const std::string second = "second version, and deliberately longer than the first";
        check(writeFileTextAtomic(target, second), "an atomic write over an existing file succeeds");
        check(readFileText(target, readBack) && readBack == second,
              "...and the new content replaces the old one completely");

        // FAILURE CASE 1: the temporary write itself cannot happen -- a directory sits where the
        // ".tmp" file needs to go, so the writeFileBytes call inside writeFileBytesAtomic fails
        // before any swap is attempted. This is the exact technique the task brief suggested: "a
        // directory where a file should be".
        std::filesystem::create_directory(tmp, ec);
        AtomicWriteError err1;
        check(!writeFileTextAtomic(target, "must never land: temp write blocked",
                                   AtomicFallback::Refuse, &err1),
              "an atomic write fails when its own temporary path is blocked by a directory");
        check(readFileText(target, readBack) && readBack == second,
              "...and the ORIGINAL is untouched -- still the second version, byte for byte");
        check(err1.op == "write temp", "...and outError says WHICH step failed: writing the temp file");
        std::filesystem::remove(tmp, ec);

        // FAILURE CASE 2: the temporary write succeeds, but the swap cannot -- something else has
        // `target` open in a way Windows will not replace out from under. aver::File's own Read
        // mode opens with FILE_SHARE_READ and nothing else (FileSystem.cpp's File::open), so a Read
        // handle held on `target` is exactly that: no FILE_SHARE_DELETE means renameFile's
        // MoveFileExW replace must fail while this handle is open.
#if defined(_WIN32)
        {
            File blocker;
            check(blocker.open(target, File::Mode::Read), "a handle is opened to block the rename");
            AtomicWriteError err2;
            check(!writeFileTextAtomic(target, "must never land: rename blocked",
                                       AtomicFallback::Refuse, &err2),
                  "an atomic write fails when the destination cannot be replaced");
            check(err2.op.rfind("swap", 0) == 0, "...and outError says the SWAP is what failed, not the temp write");
            check(isTransientFileError(err2.errorCode),
                  "...with a platform error the retry policy itself classifies as worth retrying -- "
                  "it just could not be retried AWAY because this handle never closed");
        }   // the blocking handle closes here, before the file is inspected
        check(readFileText(target, readBack) && readBack == second,
              "...and the ORIGINAL is STILL untouched after the failed rename");
        check(!std::filesystem::exists(tmp, ec),
              "...and the temporary was cleaned up rather than left as litter");

        // FAILURE CASE 3: the same blocked swap, but with AtomicFallback::DirectWrite -- and a
        // handle opened to allow this ordinary difference: FILE_SHARE_WRITE (so a second, plain
        // CreateFile for writing succeeds) WITHOUT FILE_SHARE_DELETE (so the RENAME still cannot
        // replace the directory entry out from under it). Raw CreateFileW rather than aver::File,
        // which hardcodes FILE_SHARE_READ only and cannot express this -- the same reason the
        // sparse-file case above this one already drops to the raw API. This is the case
        // writeFileBytesAtomic's DirectWrite fallback exists for: a swap that cannot complete
        // while a plain write to the same path still can.
        {
            const std::wstring wTarget = std::filesystem::path(target).wstring();
            HANDLE blocker = CreateFileW(wTarget.c_str(), GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
            check(blocker != INVALID_HANDLE_VALUE,
                  "a handle sharing READ and WRITE (but not DELETE) is opened on the destination");

            const std::string third = "third version, written directly because the swap could not land";
            AtomicWriteError err3;
            const bool wrote = writeFileTextAtomic(target, third, AtomicFallback::DirectWrite, &err3);
            if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);

            check(wrote, "with AtomicFallback::DirectWrite, the write LANDS even though the swap could not");
            check(err3.op.rfind("swap", 0) == 0,
                  "...and outError still names the swap as what failed, even on a call that overall succeeded");
            check(readFileText(target, readBack) && readBack == third,
                  "...and the file on disk is the NEW content, not the second version reverted to");
            check(!std::filesystem::exists(tmp, ec),
                  "...with no '.tmp' left behind by the fallback either");
        }
#else
        AVER_WARN("   SKIP  the rename-blocked cases need a Windows share-mode handle -- unverified here");
        ++g_skipped;
#endif
    }

    // ---- isTransientFileError: the retry policy, as a plain decision over integers ------------------
    //
    // No Windows header needed to state or check this -- see the function's own doc comment. Tested
    // directly because it IS the policy: get this wrong and the swap above either retries a
    // permissions error it can never clear (wasted time on every failure) or gives up instantly on a
    // scanner that would have let go a moment later (the whole bug this lane exists to fix).
    {
        check(isTransientFileError(32), "ERROR_SHARING_VIOLATION is worth retrying");
        check(isTransientFileError(33), "ERROR_LOCK_VIOLATION is worth retrying");
        check(isTransientFileError(5), "ERROR_ACCESS_DENIED is worth retrying");
        check(!isTransientFileError(2), "ERROR_FILE_NOT_FOUND is not -- a missing path will not appear");
        check(!isTransientFileError(3), "ERROR_PATH_NOT_FOUND is not, for the same reason");
        check(!isTransientFileError(0), "0 (no error) is not transient -- it is not an error at all");
    }

    std::filesystem::remove_all(dir, ec);

    if (g_failures == 0 && g_skipped == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else if (g_failures == 0) AVER_WARN("=== {} assertions, 0 failed, {} SKIPPED ===", g_checks, g_skipped);
    else AVER_ERROR("=== {} assertions, {} failed ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
