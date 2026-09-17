#include "pch.hpp"
#include "file_prefetch.hpp"

#include "game.hpp"
#include "log.hpp"

#include <condition_variable>
#include <deque>

namespace
{
    // Only the game's own data archives. The extensions are the ones the census saw the game read
    // on its own thread; everything else it opens is small or already warm.
    bool WorthWarming(const std::wstring& path)
    {
        const size_t dot = path.find_last_of(L'.');
        if (dot == std::wstring::npos) { return false; }
        std::wstring ext = path.substr(dot);
        for (wchar_t& c : ext) { c = static_cast<wchar_t>(towlower(c)); }
        return ext == L".pdt";
    }

    std::mutex g_Mutex;
    std::condition_variable g_Wake;
    std::deque<std::wstring> g_Queue;
    std::set<std::wstring> g_Seen;        // one warm per file, ever
    bool g_Stop = false;
    std::thread g_Worker;
    std::atomic<uint64_t> g_Files { 0 }, g_Bytes { 0 };

    void Queue(std::wstring path)
    {
        {
            std::lock_guard lock(g_Mutex);
            if (!g_Seen.insert(path).second) { return; }
            g_Queue.push_back(std::move(path));
        }
        g_Wake.notify_one();
    }

    // Reads a file through so the device is awake and the pages are resident. The data is thrown
    // away: the operating system's cache is the point, and it reclaims those pages under pressure.
    void Warm(const std::wstring& path)
    {
        const HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) { return; }
        static thread_local std::vector<char> buffer(1u << 20);
        uint64_t total = 0;
        for (;;)
        {
            DWORD got = 0;
            if (!ReadFile(h, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) || got == 0) { break; }
            total += got;
            bool stop = false;
            { std::lock_guard lock(g_Mutex); stop = g_Stop; }
            if (stop) { break; }
        }
        CloseHandle(h);
        g_Files.fetch_add(1);
        g_Bytes.fetch_add(total);
        if (mgs4e::log::Verbose())
        {
            spdlog::info("PW prefetch: warmed {:.1f} MB of a data archive ({} file(s), {:.1f} MB so far).",
                static_cast<double>(total) / (1024.0 * 1024.0), g_Files.load(), static_cast<double>(g_Bytes.load()) / (1024.0 * 1024.0));
        }
    }

    void WorkerMain()
    {
        // Below normal, so this never competes with the game for a core. The work is almost all
        // waiting on the device anyway.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        for (;;)
        {
            std::wstring next;
            {
                std::unique_lock lock(g_Mutex);
                g_Wake.wait(lock, [] { return g_Stop || !g_Queue.empty(); });
                if (g_Stop) { return; }
                next = std::move(g_Queue.front());
                g_Queue.pop_front();
            }
            Warm(next);
        }
    }

    // The game opens its archives through CreateFileW; the hook only notes the path and returns.
    using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
    CreateFileWFn g_RealCreateFileW = nullptr;
    CreateFileAFn g_RealCreateFileA = nullptr;

    HANDLE WINAPI Hooked_CreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
        DWORD disposition, DWORD flags, HANDLE templ)
    {
        const HANDLE h = g_RealCreateFileW(name, access, share, sa, disposition, flags, templ);
        // CreateFile reports ERROR_ALREADY_EXISTS through the last error even when it succeeds, so
        // queuing must not be allowed to overwrite it.
        const DWORD lastError = GetLastError();
        if (FilePrefetch::iMode >= 1 && h != INVALID_HANDLE_VALUE && name && WorthWarming(name)) { Queue(name); }
        SetLastError(lastError);
        return h;
    }

    HANDLE WINAPI Hooked_CreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
        DWORD disposition, DWORD flags, HANDLE templ)
    {
        const HANDLE h = g_RealCreateFileA(name, access, share, sa, disposition, flags, templ);
        const DWORD lastError = GetLastError();
        if (FilePrefetch::iMode >= 1 && h != INVALID_HANDLE_VALUE && name)
        {
            const int n = MultiByteToWideChar(CP_ACP, 0, name, -1, nullptr, 0);
            if (n > 1)
            {
                std::wstring wide(static_cast<size_t>(n) - 1, L'\0');
                MultiByteToWideChar(CP_ACP, 0, name, -1, wide.data(), n);
                if (WorthWarming(wide)) { Queue(std::move(wide)); }
            }
        }
        SetLastError(lastError);
        return h;
    }

    // One import table patch, the same mechanism the census uses for its own probes.
    void* PatchImport(const char* dll, const char* name, void* replacement)
    {
        const auto base = reinterpret_cast<uint8_t*>(mgs4e::game::Module());
        if (!base) { return nullptr; }
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) { return nullptr; }
        for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; imp++)
        {
            if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0) { continue; }
            auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
            auto* orig = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
            for (; orig->u1.AddressOfData; thunk++, orig++)
            {
                if (IMAGE_SNAP_BY_ORDINAL(orig->u1.Ordinal)) { continue; }
                const auto* by = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + orig->u1.AddressOfData);
                if (std::strcmp(by->Name, name) != 0) { continue; }
                DWORD old = 0;
                if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) { return nullptr; }
                void* previous = reinterpret_cast<void*>(thunk->u1.Function);
                thunk->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
                VirtualProtect(&thunk->u1.Function, sizeof(void*), old, &old);
                return previous;
            }
        }
        return nullptr;
    }

    // Mode 2: warm the voice archives at start-up, so even the first line of the first conversation
    // is hot. Only the folders that have ever produced a slow read (2026-09-17): every archive that
    // stalled the game thread lived in EXLANG\disc0_rel or MLG\disc0_rel\ADEMO, and DLCVOICE is
    // the same kind of content. Together about 1.1 GB of reclaimable page cache. MLG\disc0_rel\ADEMOHQ
    // is 6.3 GB of high quality demo the voice path never touches and is deliberately excluded, which
    // is what makes this acceptable on a handheld. Enumeration runs on the worker, not at start-up.
    void QueueVoiceArchives()
    {
        const std::filesystem::path base = mgs4e::game::Root() / "mgspw";
        const std::filesystem::path folders[] = {
            base / "EXLANG" / "disc0_rel",
            base / "MLG" / "disc0_rel" / "ADEMO",
        };
        std::error_code ec;
        size_t queued = 0;
        auto consider = [&](const std::filesystem::path& dir)
        {
            for (auto it = std::filesystem::directory_iterator(dir, std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::directory_iterator(); it.increment(ec))
            {
                if (ec) { ec.clear(); continue; }
                if (!it->is_regular_file(ec)) { continue; }
                std::wstring p = it->path().wstring();
                if (WorthWarming(p)) { Queue(std::move(p)); queued++; }
            }
        };
        for (const auto& f : folders) { consider(f); }
        // DLC voice sits under a per-region folder: mgspw\ms0\<REGION>\DLCVOICE.
        for (auto it = std::filesystem::directory_iterator(base / "ms0", std::filesystem::directory_options::skip_permission_denied, ec);
             it != std::filesystem::directory_iterator(); it.increment(ec))
        {
            if (ec) { ec.clear(); continue; }
            if (it->is_directory(ec)) { consider(it->path() / "DLCVOICE"); }
        }
        spdlog::info("PW prefetch: {} voice archive(s) queued for warming in the background.", queued);
    }
}

namespace FilePrefetch
{
    uint64_t FilesWarmed() { return g_Files.load(); }
    uint64_t BytesWarmed() { return g_Bytes.load(); }

    void Install()
    {
        if (iMode <= 0) { return; }

        g_RealCreateFileW = reinterpret_cast<CreateFileWFn>(PatchImport("KERNEL32.dll", "CreateFileW", reinterpret_cast<void*>(&Hooked_CreateFileW)));
        g_RealCreateFileA = reinterpret_cast<CreateFileAFn>(PatchImport("KERNEL32.dll", "CreateFileA", reinterpret_cast<void*>(&Hooked_CreateFileA)));
        if (!g_RealCreateFileW && !g_RealCreateFileA)
        {
            spdlog::warn("PW prefetch: neither CreateFileW nor CreateFileA could be hooked; archives will not be warmed.");
            return;
        }

        g_Worker = std::thread(WorkerMain);
        spdlog::info("PW prefetch: mode {} ({}); CreateFileW {}, CreateFileA {}.",
            iMode, iMode >= 2 ? "the voice archives from start-up" : "each archive as the game opens it",
            g_RealCreateFileW ? "hooked" : "not imported", g_RealCreateFileA ? "hooked" : "not imported");

        if (iMode >= 2) { std::thread(QueueVoiceArchives).detach(); }
    }

    void Shutdown()
    {
        if (!g_Worker.joinable()) { return; }
        { std::lock_guard lock(g_Mutex); g_Stop = true; }
        g_Wake.notify_all();
        g_Worker.join();
    }
}
