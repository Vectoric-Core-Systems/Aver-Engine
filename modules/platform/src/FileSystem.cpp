// File I/O, well-known directories and the native open-file dialog.

#include "aver/platform/FileSystem.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#if defined(_WIN32)
#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#else
// The POSIX half of aver::File. pread/pwrite rather than lseek+read, so the handle carries no
// implicit position and two calls cannot race each other's seek.
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace aver {

#if defined(_WIN32)
namespace {

// Converts a wide string to UTF-8.
std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<usize>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}

// Converts a UTF-8 string to a wide string.
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(static_cast<usize>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), len);
    return w;
}

// Asks the shell for a known folder's path. Empty on failure.
std::string knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw)) || !raw) return {};
    std::string s = narrow(raw);
    CoTaskMemFree(raw);
    return s;
}

// Bounded: three attempts total, a few milliseconds apart. This is sized for "another process had
// it open for a moment" (a virus scanner's on-access scan of the file this process just finished
// writing, an indexer, a backup agent), not for anything that needs seconds to clear -- a lock that
// outlives this is not a lock this function's caller can wait out anyway, and the whole retry only
// runs on the failure path in the first place.
constexpr int kSwapAttempts = 3;
constexpr DWORD kSwapRetryDelayMs = 10;

// Swaps `tmp` into `path`, trying MoveFileExW and then ReplaceFileW on each pass, and retrying while
// the failure looks transient. Returns true the instant either call succeeds. On total failure,
// `outOp`/`outErr` carry the LAST attempt's operation name and GetLastError, so a caller can log
// something more useful than "could not write".
bool swapWithRetry(const std::wstring& wTmp, const std::wstring& wPath,
                   std::string& outOp, DWORD& outErr) {
    for (int attempt = 0; attempt < kSwapAttempts; ++attempt) {
        if (attempt > 0) Sleep(kSwapRetryDelayMs);

        if (MoveFileExW(wTmp.c_str(), wPath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            return true;
        }
        outOp = "swap (MoveFileExW)";
        outErr = GetLastError();

        // ReplaceFileW is the platform's OWN "replace a file something else may have open"
        // primitive, so it is worth trying on the SAME pass before sleeping and trying MoveFileExW
        // again. It refuses a `path` that does not exist yet with ERROR_FILE_NOT_FOUND -- the
        // ordinary first-save case, not the sharing failure this is hunting -- so that specific code
        // does not clobber the more informative one MoveFileExW just reported.
        if (ReplaceFileW(wPath.c_str(), wTmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS,
                         nullptr, nullptr) != 0) {
            return true;
        }
        const DWORD replaceErr = GetLastError();
        if (replaceErr != ERROR_FILE_NOT_FOUND) { outOp = "swap (ReplaceFileW)"; outErr = replaceErr; }

        if (!isTransientFileError(static_cast<u32>(outErr))) break;   // a real reason: sleeping will not fix it
    }
    return false;
}

} // namespace
#endif

// See the header: which platform error codes are worth a bounded retry. Defined here, outside the
// Windows-only block above, so it needs no Windows header to compile or to test -- the three codes
// it names are plain integers from the stable Win32 error ABI, not live values this function has to
// ask the OS for.
bool isTransientFileError(u32 platformErrorCode) {
    constexpr u32 kErrorSharingViolation = 32;
    constexpr u32 kErrorLockViolation = 33;
    constexpr u32 kErrorAccessDenied = 5;
    return platformErrorCode == kErrorSharingViolation || platformErrorCode == kErrorLockViolation ||
          platformErrorCode == kErrorAccessDenied;
}

// The directory containing the running executable.
std::string executableDir() {
#if defined(_WIN32)
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring w(buf, n);
    const auto p = w.find_last_of(L"\\/");
    if (p != std::wstring::npos) w = w.substr(0, p);
    return narrow(w);
#else
    return ".";
#endif
}

// The per-user, per-machine state directory.
std::string userDataDir() {
#if defined(_WIN32)
    const std::string local = knownFolder(FOLDERID_LocalAppData);
    if (!local.empty()) return local + "\\AverEngine";
#endif
    return executableDir();
}

// The user's Documents folder.
std::string documentsDir() {
#if defined(_WIN32)
    const std::string docs = knownFolder(FOLDERID_Documents);
    if (!docs.empty()) return docs;
#endif
    return executableDir();
}

// True if the path exists.
bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && !ec;
}

// True if the path exists and is a directory.
bool directoryExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec) && !ec;
}

// Creates the directory and every missing parent. True if it exists afterwards.
bool createDirectories(const std::string& path) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec && std::filesystem::is_directory(path, ec);
}

// Reads a whole file into `out`. False if it cannot be read.
namespace {
FileTraceFn g_trace = nullptr;
void* g_traceUser = nullptr;
}

void setFileTrace(FileTraceFn fn, void* user) { g_trace = fn; g_traceUser = user; }

void traceFileOpen(const std::string& path) { if (g_trace) g_trace(path.c_str(), g_traceUser); }

bool readFileBytes(const std::string& path, std::vector<u8>& out) {
    if (g_trace) g_trace(path.c_str(), g_traceUser);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<usize>(size));
    if (size > 0 && !f.read(reinterpret_cast<char*>(out.data()), size)) return false;
    return true;
}

// Reads a whole file into `out` as text. False if it cannot be read.
bool readFileText(const std::string& path, std::string& out) {
    // Traced on ENTRY, before the open is attempted, so a path that fails to open is still
    // reported. A fallback that misses is exactly as informative as one that hits: it says the
    // package looked outside itself.
    if (g_trace) g_trace(path.c_str(), g_traceUser);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamoff size = f.tellg();
    if (size < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<usize>(size));
    if (size > 0 && !f.read(out.data(), size)) return false;
    return true;
}

// Writes `size` bytes to the file, truncating it. False on failure.
bool writeFileBytes(const std::string& path, const void* data, usize size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (size > 0) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(f);
}

// Writes `text` to the file, truncating it. False on failure.
bool writeFileText(const std::string& path, const std::string& text) {
    return writeFileBytes(path, text.data(), text.size());
}

// Deletes a file. True if it is gone afterwards, including if it never existed.
bool deleteFile(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    // remove() returns false for "was not there", which is a SUCCESS by this function's contract --
    // so the answer is the resulting state, not the return value.
    return !std::filesystem::exists(path, ec);
}

// Renames `from` to `to`, replacing `to` if present.
bool renameFile(const std::string& from, const std::string& to) {
#if defined(_WIN32)
    // MoveFileExW with REPLACE_EXISTING rather than std::filesystem::rename: the standard one is
    // specified to fail when the destination exists on some implementations, and replace-in-place is
    // exactly the operation write-to-temp-then-swap needs. WRITE_THROUGH makes the rename itself
    // durable before returning, so a crash cannot leave the directory entry unwritten.
    //
    // RETRIED, via swapWithRetry, rather than one shot -- see that function's comment. Every caller
    // of renameFile (OcSave.cpp, Settings.cpp, RegionFile.cpp, and writeFileBytesAtomic below) gets
    // this for free with no change on their part: it only improves the odds a swap that COULD
    // succeed does, and behaves exactly as before when it genuinely cannot.
    std::string op; DWORD err = 0;
    return swapWithRetry(widen(from), widen(to), op, err);
#else
    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    return !ec;
#endif
}

// Writes `size` bytes to `path` by way of a temporary, so `path` itself is only ever touched by
// the rename that swaps a COMPLETE replacement into place. See the header comment for the full
// argument; this is that same write-to-temp-then-swap pattern aver::fmt::saveOcSave and
// aver_settings_flush already ship with, factored out to one place.
bool writeFileBytesAtomic(const std::string& path, const void* data, usize size,
                         AtomicFallback fallback, AtomicWriteError* outError) {
    if (outError) *outError = AtomicWriteError{};

    const std::string tmp = path + ".tmp";
    // The temporary is written with the plain, truncating writeFileBytes -- truncate-on-open is
    // harmless here because `tmp` is not a file anything else depends on; the SWAP below is what
    // makes `path` itself safe, not this write.
    if (!writeFileBytes(tmp, data, size)) {
        if (outError) outError->op = "write temp";
        // DirectWrite exists for when the SWAP cannot complete, not when the write into `tmp` itself
        // fails -- if the destination directory will not accept a new file at all, it will not accept
        // one at `path` either, so there is nothing this fallback could still land. Fall through to
        // the ordinary failure return.
        return false;
    }

#if defined(_WIN32)
    std::string op; DWORD err = 0;
    if (!swapWithRetry(widen(tmp), widen(path), op, err)) {
        if (outError) { outError->op = op; outError->errorCode = static_cast<u32>(err); }
        // The temporary is removed on a failed swap so a later attempt does not inherit a stale
        // one and the save directory does not accumulate ".tmp" debris. `path` is untouched
        // either way -- the swap never completed, so whatever was there (or was not) still is.
        deleteFile(tmp);
        if (fallback == AtomicFallback::DirectWrite) {
            // THE TRADE, spelled out because it is a real one. The swap exists to survive THIS
            // PROCESS crashing mid-write -- writeFileBytes below has the ordinary truncate-on-open
            // hazard, and a crash between the truncate and the last byte landing would leave `path`
            // corrupted rather than merely stale. What just failed above is a DIFFERENT risk: a
            // THIRD PARTY (a scanner, an indexer, a backup agent) holding `path` open long enough
            // that even the retried swap could not replace it. The two risks are independent, and a
            // caller opting into DirectWrite is one that has already decided losing the first
            // guarantee is better than the preference never reaching disk at all -- see the enum's
            // doc comment for who that is and, just as importantly, who must never do this.
            return writeFileBytes(path, data, size);
        }
        return false;
    }
    return true;
#else
    // POSIX rename() has no share-mode concept -- replacing a file someone else has open just
    // unlinks the old inode from under their still-valid handle -- so there is no swap-retry story
    // here, and DirectWrite's only use on this path is the rare case where `tmp` and `path` do not
    // share a filesystem (EXDEV) and a direct write can still land where a rename across devices
    // cannot.
    if (!renameFile(tmp, path)) {
        if (outError) { outError->op = "swap"; outError->errorCode = static_cast<u32>(errno); }
        deleteFile(tmp);
        if (fallback == AtomicFallback::DirectWrite) return writeFileBytes(path, data, size);
        return false;
    }
    return true;
#endif
}

// Writes `text` to the file safely; see writeFileBytesAtomic.
bool writeFileTextAtomic(const std::string& path, const std::string& text,
                        AtomicFallback fallback, AtomicWriteError* outError) {
    return writeFileBytesAtomic(path, text.data(), text.size(), fallback, outError);
}

// See the header: FormatMessage on Windows, strerror elsewhere.
std::string describePlatformError(u32 code) {
    if (code == 0) return {};
#if defined(_WIN32)
    LPWSTR buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(code), 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    if (n == 0 || !buf) return {};
    std::wstring w(buf, n);
    LocalFree(buf);
    // FormatMessage's text ends in "\r\n" (or a trailing period plus that) -- trimmed so it reads as
    // one clause in a log line rather than leaving a blank line dangling after it.
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) w.pop_back();
    return narrow(w);
#else
    const char* s = std::strerror(static_cast<int>(code));
    return s ? std::string(s) : std::string();
#endif
}

// ---- File ---------------------------------------------------------------------------------------

File::~File() { close(); }

File::File(File&& other) noexcept {
#if defined(_WIN32)
    handle_ = other.handle_; other.handle_ = nullptr;
#else
    fd_ = other.fd_; other.fd_ = -1;
#endif
    writable_ = other.writable_; other.writable_ = false;
}

File& File::operator=(File&& other) noexcept {
    if (this == &other) return *this;
    close();
#if defined(_WIN32)
    handle_ = other.handle_; other.handle_ = nullptr;
#else
    fd_ = other.fd_; other.fd_ = -1;
#endif
    writable_ = other.writable_; other.writable_ = false;
    return *this;
}

bool File::open(const std::string& path, Mode mode) {
    close();
    // Traced in EVERY mode, including Create -- see the note on setFileTrace in the header for why
    // a write-only open is traced too.
    traceFileOpen(path);
    writable_ = mode != Mode::Read;
#if defined(_WIN32)
    const DWORD access = mode == Mode::Read ? GENERIC_READ : (GENERIC_READ | GENERIC_WRITE);
    // OPEN_EXISTING / OPEN_ALWAYS, never CREATE_ALWAYS or TRUNCATE_EXISTING: not truncating is the
    // point of this whole type.
    const DWORD disp = mode == Mode::Create ? OPEN_ALWAYS : OPEN_EXISTING;
    // FILE_SHARE_READ so a second reader (a tool, the editor) can look while this handle is open;
    // deliberately NOT FILE_SHARE_WRITE, so two writers cannot interleave into one region file.
    const HANDLE h = CreateFileW(widen(path).c_str(), access, FILE_SHARE_READ, nullptr, disp,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { writable_ = false; return false; }
    handle_ = h;
    return true;
#else
    int flags = mode == Mode::Read ? O_RDONLY : O_RDWR;
    if (mode == Mode::Create) flags |= O_CREAT;
    const int fd = ::open(path.c_str(), flags, 0644);
    if (fd < 0) { writable_ = false; return false; }
    fd_ = fd;
    return true;
#endif
}

void File::close() {
#if defined(_WIN32)
    if (handle_) { CloseHandle(static_cast<HANDLE>(handle_)); handle_ = nullptr; }
#else
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
    writable_ = false;
}

bool File::isOpen() const {
#if defined(_WIN32)
    return handle_ != nullptr;
#else
    return fd_ >= 0;
#endif
}

i64 File::size() const {
    if (!isOpen()) return -1;
#if defined(_WIN32)
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(static_cast<HANDLE>(handle_), &li)) return -1;
    return static_cast<i64>(li.QuadPart);
#else
    struct stat st{};
    if (fstat(fd_, &st) != 0) return -1;
    return static_cast<i64>(st.st_size);
#endif
}

bool File::readAt(u64 offset, void* out, usize bytes) const {
    if (!isOpen()) return false;
    if (bytes == 0) return true;
    if (!out) return false;
    auto* dst = static_cast<u8*>(out);
    usize done = 0;
    // LOOPED, because both platforms may legitimately return fewer bytes than asked for on a single
    // call. A short read is only an ERROR once it stops making progress -- which is the end of the
    // file, and which this function's contract says must fail rather than half-succeed.
    while (done < bytes) {
        const usize want = bytes - done;
#if defined(_WIN32)
        OVERLAPPED ov{};
        const u64 at = offset + done;
        ov.Offset     = static_cast<DWORD>(at & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        const DWORD chunk = want > 0x7FFFFFFFu ? 0x7FFFFFFFu : static_cast<DWORD>(want);
        DWORD got = 0;
        if (!ReadFile(static_cast<HANDLE>(handle_), dst + done, chunk, &got, &ov)) return false;
        if (got == 0) return false;   // end of file before `bytes` were satisfied
        done += got;
#else
        const ssize_t got = pread(fd_, dst + done, want, static_cast<off_t>(offset + done));
        if (got < 0) { if (errno == EINTR) continue; return false; }
        if (got == 0) return false;
        done += static_cast<usize>(got);
#endif
    }
    return true;
}

bool File::writeAt(u64 offset, const void* data, usize bytes) {
    if (!isOpen() || !writable_) return false;
    if (bytes == 0) return true;
    if (!data) return false;
    const auto* src = static_cast<const u8*>(data);
    usize done = 0;
    while (done < bytes) {
        const usize want = bytes - done;
#if defined(_WIN32)
        OVERLAPPED ov{};
        const u64 at = offset + done;
        ov.Offset     = static_cast<DWORD>(at & 0xFFFFFFFFull);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        const DWORD chunk = want > 0x7FFFFFFFu ? 0x7FFFFFFFu : static_cast<DWORD>(want);
        DWORD put = 0;
        if (!WriteFile(static_cast<HANDLE>(handle_), src + done, chunk, &put, &ov)) return false;
        if (put == 0) return false;
        done += put;
#else
        const ssize_t put = pwrite(fd_, src + done, want, static_cast<off_t>(offset + done));
        if (put < 0) { if (errno == EINTR) continue; return false; }
        if (put == 0) return false;
        done += static_cast<usize>(put);
#endif
    }
    return true;
}

bool File::setSize(u64 bytes) {
    if (!isOpen() || !writable_) return false;
#if defined(_WIN32)
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(bytes);
    if (!SetFilePointerEx(static_cast<HANDLE>(handle_), li, nullptr, FILE_BEGIN)) return false;
    return SetEndOfFile(static_cast<HANDLE>(handle_)) != 0;
#else
    return ftruncate(fd_, static_cast<off_t>(bytes)) == 0;
#endif
}

bool File::sync() {
    if (!isOpen()) return false;
    if (!writable_) return true;   // nothing of ours can be in flight
#if defined(_WIN32)
    return FlushFileBuffers(static_cast<HANDLE>(handle_)) != 0;
#else
    return fsync(fd_) == 0;
#endif
}

// Shows the shell's open-file dialog. False if cancelled or unavailable.
bool openFileDialog(const std::string& title, const std::string& filterLabel,
                    const std::string& spec, const std::string& initialDir, std::string& out) {
#if defined(_WIN32)
    // COM is initialised per call and undone, so nothing else inherits an apartment.
    const HRESULT ci = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool weInitialised = SUCCEEDED(ci);
    if (ci == RPC_E_CHANGED_MODE) { /* someone else owns the apartment; the dialog still works */ }
    else if (!weInitialised) return false;

    bool ok = false;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg))) && dlg) {
        const std::wstring wTitle = widen(title), wLabel = widen(filterLabel), wSpec = widen(spec);
        const COMDLG_FILTERSPEC filters[] = {{wLabel.c_str(), wSpec.c_str()}, {L"All files", L"*.*"}};
        dlg->SetTitle(wTitle.c_str());
        dlg->SetFileTypes(2, filters);
        if (!initialDir.empty()) {
            IShellItem* start = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(widen(initialDir).c_str(), nullptr, IID_PPV_ARGS(&start))) && start) {
                dlg->SetFolder(start);
                start->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR raw = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                    out = narrow(raw);
                    CoTaskMemFree(raw);
                    ok = true;
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (weInitialised) CoUninitialize();
    return ok;
#else
    (void)title; (void)filterLabel; (void)spec; (void)initialDir; (void)out;
    return false;
#endif
}

} // namespace aver
