#include "pch.hpp"

#include "render_pipeline.hpp"

#include "aspect_ratio.hpp"
#include "game.hpp"
#include "mem.hpp"
#include "graphics_settings.hpp"
#include "log.hpp"
#include "compat.hpp"
#include "settings_keys.hpp"

#include <d3d12.h>
#include <intrin.h>
#include <Zydis/Zydis.h>
#include <set>
#include <map>
#include <mutex>
#include <thread>
#include <format>
#include <atomic>


namespace
{
    // ======================= D3D12 render target diagnostics =======================
    // The game picks its backend at runtime (there's a D3D11/D3D12 toggle in its
    // settings) and resolves it with LoadLibrary/GetProcAddress rather than importing
    // it, so there's nothing in mgs4.exe's import table to hook. Instead we load
    // d3d12.dll ourselves and hook the exported D3D12CreateDevice, then hook the
    // resource creation slots on the device vtable once the game creates it.

    SafetyHookInline D3D12CreateDevice_hook {};
    SafetyHookInline CreateCommittedResource_hook {};
    SafetyHookInline CreatePlacedResource_hook {};

    // ID3D12Device vtable slots (stable, part of the public COM ABI).
    constexpr size_t kCreateSamplerSlot = 22;
    constexpr size_t kCreateCommittedResourceSlot = 27;
    constexpr size_t kCreatePlacedResourceSlot = 29;

    // The newer allocation entry points. Hooking only the original CreateCommittedResource
    // misses every buffer allocated through ID3D12Device4/8, which is how the UI vertex pool
    // is created - it never appeared in our bookkeeping, so nothing that needed to read vertex
    // data ever worked.
    constexpr size_t kCreateCommittedResource1Slot = 53;   // ID3D12Device4
    constexpr size_t kCreateCommittedResource2Slot = 68;   // ID3D12Device8

    std::mutex g_ResourceLogMutex;
    std::unordered_set<uint64_t> g_LoggedResources;

    // Walks the stack and reports only the frames that live inside mgs4.exe, as
    // module-relative offsets. These are the addresses worth looking at in a
    // disassembler - they're the game code that asked for the allocation.
    std::string DescribeGameCallers()
    {
        void* frames[24] {};
        const USHORT captured = RtlCaptureStackBackTrace(1, 24, frames, nullptr);

        std::ostringstream out;
        int printed = 0;
        for (USHORT i = 0; i < captured && printed < 5; i++)
        {
            const auto address = reinterpret_cast<uintptr_t>(frames[i]);

            HMODULE owner = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(address), &owner) || owner != mgs4e::game::Module())
            {
                continue;
            }

            out << " <- mgs4.exe+0x" << std::hex << std::uppercase << (address - reinterpret_cast<uintptr_t>(owner));
            printed++;
        }
        return out.str();
    }

    // The one stack frame that distinguishes UI draw paths, as a bare RVA.
    //
    // Frame 0 is always the shared submit helper (+79F42D); frame 1 is the caller that decides
    // what is being drawn - +7A5DA3 and +7A70BE are the two seen during gameplay. That single
    // frame is what a per-draw log needs; the full DescribeGameCallers string is far too long
    // to attach to a couple of thousand lines.
    std::string UiDrawCallSite()
    {
        void* frames[8] {};
        const USHORT captured = RtlCaptureStackBackTrace(1, 8, frames, nullptr);

        int seen = 0;
        for (USHORT i = 0; i < captured; i++)
        {
            const auto address = reinterpret_cast<uintptr_t>(frames[i]);

            HMODULE owner = nullptr;
            if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(address), &owner) || owner != mgs4e::game::Module())
            {
                continue;
            }

            if (++seen == 2)
            {
                return std::format("{:X}", address - reinterpret_cast<uintptr_t>(owner));
            }
        }
        return "?";
    }

    // The engine's config init (mgs4.exe+0x3BD90) parses these four keys straight into
    // fixed globals at the very start of .data:
    //   render.bufferSizeX -> +0x1B00000     render.windowSizeX -> +0x1B00460
    //   render.bufferSizeY -> +0x1B00004     render.windowSizeY -> +0x1B00464
    // "bufferSize" is the internal render buffer (what the DynamicResolution debug panel
    // calls "Buffer"), and is genuinely separate from the window size.
    constexpr uintptr_t kRvaRenderBufferSizeX = 0x1B00000;
    constexpr uintptr_t kRvaRenderBufferSizeY = 0x1B00004;
    constexpr uintptr_t kRvaRenderWindowSizeX = 0x1B00460;
    constexpr uintptr_t kRvaRenderWindowSizeY = 0x1B00464;

    int32_t* GameInt(uintptr_t rva)
    {
        return reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + rva);
    }

    // Watches the render size globals and reports every change with a timestamp, so we
    // can tell whether our override survives to renderer init or gets written back.
    void StartRenderSizeMonitor()
    {
        std::thread([]
            {
                int32_t lastBufX = 0, lastBufY = 0, lastWinX = 0, lastWinY = 0;
                bool first = true;

                for (int i = 0; i < 1200; i++) // ~2 minutes at 100ms
                {
                    const int32_t bufX = *GameInt(kRvaRenderBufferSizeX);
                    const int32_t bufY = *GameInt(kRvaRenderBufferSizeY);
                    const int32_t winX = *GameInt(kRvaRenderWindowSizeX);
                    const int32_t winY = *GameInt(kRvaRenderWindowSizeY);

                    if (first || bufX != lastBufX || bufY != lastBufY || winX != lastWinX || winY != lastWinY)
                    {
                        spdlog::info("MGS4: RenderSize monitor: bufferSize={}x{}  windowSize={}x{}",
                            bufX, bufY, winX, winY);
                        lastBufX = bufX; lastBufY = bufY; lastWinX = winX; lastWinY = winY;
                        first = false;
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }

                spdlog::info("MGS4: RenderSize monitor: finished.");
            }).detach();
    }

    std::string ReadAsciiAt(uintptr_t pointer);
    std::string DescribeNearbyStrings(const uint8_t* at);
    void DumpNamedTable(uintptr_t startRva, uintptr_t endRva);
    int* ResolveRenderConfigGlobal(const char* key);
    void StartStateLogHotkey();
    void RegisterUploadBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
        const D3D12_RESOURCE_DESC* desc, void** resource);

    // Upload buffers guarded from creation, so the first CPU write to each identifies itself.
    struct UploadGuard
    {
        uintptr_t base;
        size_t size;
        uintptr_t writer;   // RVA of the instruction that first wrote, 0 until it does
        bool guarded;
    };

    std::vector<UploadGuard> g_UploadGuards;

    // Whether the guards fire at all. "No writes" means nothing without this: it could equally
    // mean PAGE_GUARD is not taking effect on driver-mapped memory.
    std::atomic<int> g_UploadGuardReads { 0 };
    std::mutex g_UploadGuardMutex;

    void RegisterUploadGuard(uintptr_t base, size_t size)
    {
        // Cap it: each buffer costs one fault, but the bookkeeping should not grow without
        // bound in a long session.
        constexpr size_t kMaxGuarded = 256;

        std::lock_guard lock(g_UploadGuardMutex);
        if (g_UploadGuards.size() >= kMaxGuarded || size == 0)
        {
            return;
        }

        DWORD previous = 0;
        const bool ok = VirtualProtect(reinterpret_cast<void*>(base), size,
            PAGE_READWRITE | PAGE_GUARD, &previous) != 0;

        // Report the first few either way. Upload heaps are often write-combined, and if
        // PAGE_GUARD is rejected on them then "no buffer matched" would mean nothing at all -
        // exactly the ambiguity that has wasted runs before.
        static int reported = 0;
        if (reported < 5)
        {
            reported++;
            spdlog::info("MGS4: Upload guard: {} 0x{:X} ({} bytes){}",
                ok ? "armed" : "FAILED to arm on", base, size,
                ok ? "" : std::format(" - error {}", GetLastError()));
        }

        if (!ok)
        {
            return;
        }

        g_UploadGuards.push_back(UploadGuard { base, size, 0, true });
    }

    // mgs4.exe's .data section has a ~574MB virtual size against a ~2.5MB raw size,
    // i.e. the engine keeps its state in one enormous zero-initialised global blob.
    // The render resolution the renderer actually uses lives somewhere in there, so
    // scan the image for adjacent (width, height) int pairs at the moment we know the
    // renderer has just used them, and report their module-relative offsets.
    void ScanForResolutionGlobals(int width, int height)
    {
        MODULEINFO moduleInfo {};
        if (!GetModuleInformation(GetCurrentProcess(), mgs4e::game::Module(), &moduleInfo, sizeof(moduleInfo)))
        {
            spdlog::error("MGS4: Resolution scan: GetModuleInformation failed.");
            return;
        }

        auto* const imageStart = static_cast<uint8_t*>(moduleInfo.lpBaseOfDll);
        auto* const imageEnd = imageStart + moduleInfo.SizeOfImage;

        spdlog::info("MGS4: Resolution scan: searching {} MB of mgs4.exe for adjacent ({}, {}) int pairs.",
            moduleInfo.SizeOfImage / (1024 * 1024), width, height);

        int found = 0;
        constexpr int kMaxReported = 60;

        uint8_t* cursor = imageStart;
        while (cursor < imageEnd && found < kMaxReported)
        {
            MEMORY_BASIC_INFORMATION mbi {};
            if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize == 0)
            {
                break;
            }

            auto* regionStart = static_cast<uint8_t*>(mbi.BaseAddress);
            uint8_t* regionEnd = regionStart + mbi.RegionSize;
            if (regionEnd > imageEnd)
            {
                regionEnd = imageEnd;
            }

            // Writable pages only - read-only .rdata is full of power-of-two lookup
            // tables that match these values by coincidence.
            const bool readable = mbi.State == MEM_COMMIT
                && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0
                && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;

            if (readable)
            {
                uint8_t* scan = (cursor > regionStart) ? cursor : regionStart;
                scan = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(scan) & ~static_cast<uintptr_t>(3));

                for (; scan + 8 <= regionEnd && found < kMaxReported; scan += 4)
                {
                    if (*reinterpret_cast<const int32_t*>(scan) == width
                        && *reinterpret_cast<const int32_t*>(scan + 4) == height)
                    {
                        found++;

                        // Dump the surrounding ints so we can recognise what structure
                        // the value lives in (a render target definition, a settings
                        // block, and so on).
                        std::string context;
                        for (int i = -8; i <= 12; i++)
                        {
                            const auto* neighbour = reinterpret_cast<const int32_t*>(scan) + i;
                            if (!mgs4e::mem::Readable(neighbour, sizeof(int32_t)))
                            {
                                continue;
                            }
                            context += (i == 0 ? std::format(" [{}]", *neighbour) : std::format(" {}", *neighbour));
                        }

                        spdlog::info("MGS4: Resolution scan: candidate at mgs4.exe+0x{:X} context:{}{}",
                            static_cast<uintptr_t>(scan - imageStart), context, DescribeNearbyStrings(scan));
                    }
                }
            }

            cursor = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        }

        spdlog::info("MGS4: Resolution scan: complete, {} candidate(s){}.",
            found, found >= kMaxReported ? " (capped)" : "");
    }

    // Same walk as above, but looking for a single int value rather than a pair. Used to
    // hunt for the shadow buffer size, which the engine stores as one dimension.
    void ScanForSingleInt(int value, const char* label)
    {
        MODULEINFO moduleInfo {};
        if (!GetModuleInformation(GetCurrentProcess(), mgs4e::game::Module(), &moduleInfo, sizeof(moduleInfo)))
        {
            return;
        }

        auto* const imageStart = static_cast<uint8_t*>(moduleInfo.lpBaseOfDll);
        auto* const imageEnd = imageStart + moduleInfo.SizeOfImage;

        int found = 0;
        constexpr int kMaxReported = 200;
        std::string hits;

        uint8_t* cursor = imageStart;
        while (cursor < imageEnd && found < kMaxReported)
        {
            MEMORY_BASIC_INFORMATION mbi {};
            if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize == 0)
            {
                break;
            }

            auto* regionStart = static_cast<uint8_t*>(mbi.BaseAddress);
            uint8_t* regionEnd = regionStart + mbi.RegionSize;
            if (regionEnd > imageEnd)
            {
                regionEnd = imageEnd;
            }

            // Only writable data pages - the value we want is engine state, not a code immediate.
            const bool writableData = mbi.State == MEM_COMMIT
                && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0
                && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;

            if (writableData)
            {
                uint8_t* scan = (cursor > regionStart) ? cursor : regionStart;
                scan = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(scan) & ~static_cast<uintptr_t>(3));

                for (; scan + 4 <= regionEnd && found < kMaxReported; scan += 4)
                {
                    if (*reinterpret_cast<const int32_t*>(scan) == value)
                    {
                        found++;
                        hits += std::format(" +0x{:X}", static_cast<uintptr_t>(scan - imageStart));
                    }
                }
            }

            cursor = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        }

        spdlog::info("MGS4: {} scan: {} == {} candidate(s){}:{}",
            label, value, found, found >= kMaxReported ? " (capped)" : "", hits);
    }

    // Reads a NUL-terminated printable ASCII string, or returns empty if the address
    // doesn't hold one. Used to turn raw pointers in engine tables into readable names.
    std::string ReadAsciiAt(uintptr_t pointer)
    {
        const auto* text = reinterpret_cast<const char*>(pointer);
        if (pointer < 0x10000 || !mgs4e::mem::Readable(text, 4))
        {
            return {};
        }

        std::string name;
        for (int c = 0; c < 64; c++)
        {
            if (!mgs4e::mem::Readable(text + c, 1))
            {
                break;
            }
            const char ch = text[c];
            if (ch == '\0')
            {
                break;
            }
            if (ch < 0x20 || ch > 0x7E)
            {
                return {};
            }
            name += ch;
        }

        return name.size() >= 3 ? name : std::string {};
    }

    // Entries in the engine's render target table appear to carry a pointer just before
    // their width/height. If those point at readable ASCII they're almost certainly the
    // target's name, which identifies each entry far better than guessing from numbers.
    std::string DescribeNearbyStrings(const uint8_t* at)
    {
        std::string out;

        for (int i = -12; i <= 2; i++)
        {
            const auto* slot = reinterpret_cast<const int32_t*>(at) + i;
            if (!mgs4e::mem::Readable(slot, sizeof(uintptr_t)))
            {
                continue;
            }

            uintptr_t pointer = 0;
            std::memcpy(&pointer, slot, sizeof(pointer));

            if (const std::string name = ReadAsciiAt(pointer); !name.empty())
            {
                out += std::format("  [{}]=\"{}\"", i, name);
            }
        }

        return out;
    }

    // Walks a region looking for "pointer to a name followed by two ints" - the shape the
    // render target table entries appear to have - and reports each as name + dimensions.
    // Stride-agnostic, so it works without having to pin down the exact struct layout.
    void DumpNamedTable(uintptr_t startRva, uintptr_t endRva)
    {
        auto* const base = reinterpret_cast<uint8_t*>(mgs4e::game::Module());
        int printed = 0;

        spdlog::info("MGS4: Named table dump, mgs4.exe+0x{:X}..+0x{:X}:", startRva, endRva);

        for (uintptr_t offset = startRva; offset + 16 <= endRva && printed < 120; offset += 4)
        {
            const auto* slot = base + offset;
            if (!mgs4e::mem::Readable(slot, 16))
            {
                continue;
            }

            uintptr_t pointer = 0;
            std::memcpy(&pointer, slot, sizeof(pointer));

            const std::string name = ReadAsciiAt(pointer);
            if (name.empty())
            {
                continue;
            }

            int32_t first = 0;
            int32_t second = 0;
            std::memcpy(&first, slot + 8, sizeof(first));
            std::memcpy(&second, slot + 12, sizeof(second));

            spdlog::info("MGS4:   +0x{:X}  \"{}\"  {} x {}", offset, name, first, second);
            printed++;
        }

        spdlog::info("MGS4: Named table dump complete, {} entries{}.", printed, printed >= 120 ? " (capped)" : "");
    }

    void LogResourceDesc(const char* source, const D3D12_RESOURCE_DESC* desc)
    {
        if (!RenderPipeline::bLogRenderTargetAllocations || !desc
            || desc->Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D)
        {
            return;
        }

        // Only care about things the GPU renders into, not streamed asset textures.
        constexpr UINT kRenderableFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
            | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
            | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        if ((desc->Flags & kRenderableFlags) == 0 || desc->Width < 256 || desc->Height < 256)
        {
            return;
        }

        // Dedupe so a per-frame allocation doesn't flood the log; each distinct
        // size/format/flags combination is reported once.
        const uint64_t key = (static_cast<uint64_t>(desc->Width & 0xFFFF) << 48)
            | (static_cast<uint64_t>(desc->Height & 0xFFFF) << 32)
            | (static_cast<uint64_t>(desc->Format) << 16)
            | static_cast<uint64_t>(desc->Flags & 0xFFFF);

        {
            std::lock_guard lock(g_ResourceLogMutex);
            if (!g_LoggedResources.insert(key).second)
            {
                return;
            }
        }

        spdlog::info("MGS4: D3D12 RT [{}] {}x{} fmt={} mips={} samples={} flags=0x{:X}{}",
            source,
            static_cast<uint64_t>(desc->Width),
            desc->Height,
            static_cast<int>(desc->Format),
            desc->MipLevels,
            desc->SampleDesc.Count,
            static_cast<uint32_t>(desc->Flags),
            DescribeGameCallers());

        // The first full-size render target tells us the resolution the renderer
        // settled on. Scan for it now, while the globals that produced it are live.
        static bool bScanned = false;
        if (!bScanned && desc->Width >= 2560 && desc->Height >= 1440)
        {
            bScanned = true;
            ScanForResolutionGlobals(static_cast<int>(desc->Width), static_cast<int>(desc->Height));
        }

        // The shadow atlas is the R32_TYPELESS depth target - the one buffer that did not
        // scale with the render resolution. Scan for whatever drives its size.
        static bool bShadowScanned = false;
        if (!bShadowScanned
            && desc->Format == DXGI_FORMAT_R32_TYPELESS
            && (desc->Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0)
        {
            bShadowScanned = true;
            const int shadowW = static_cast<int>(desc->Width);
            const int shadowH = desc->Height;
            spdlog::info("MGS4: Shadow atlas detected at {}x{}, scanning for its source.", shadowW, shadowH);
            ScanForResolutionGlobals(shadowW, shadowH);

            // The (2048,4096) pair landed inside what looks like the engine's render
            // target table around +0x23FE1160. Dump the surrounding region by name so we
            // can identify the shadow entry rather than infer it from raw numbers.
            DumpNamedTable(0x23FDE000, 0x23FE3000);
        }
    }

    // A failed allocation is far more interesting than a successful one: the engine may
    // not report it, and a texture that silently fails to be created shows up in-game as
    // something simply missing. Always log these, even with diagnostics otherwise off.
    void LogAllocationFailure(const char* source, const D3D12_RESOURCE_DESC* desc, HRESULT result)
    {
        if (SUCCEEDED(result) || !desc)
        {
            return;
        }

        spdlog::error("MGS4: D3D12 {} FAILED (0x{:08X}): {}x{} fmt={} mips={} samples={} flags=0x{:X}{}",
            source,
            static_cast<uint32_t>(result),
            static_cast<uint64_t>(desc->Width),
            desc->Height,
            static_cast<int>(desc->Format),
            desc->MipLevels,
            desc->SampleDesc.Count,
            static_cast<uint32_t>(desc->Flags),
            DescribeGameCallers());
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateCommittedResource(ID3D12Device* self,
        const D3D12_HEAP_PROPERTIES* heapProperties, D3D12_HEAP_FLAGS heapFlags,
        const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
        const D3D12_CLEAR_VALUE* optimizedClearValue, REFIID riid, void** resource)
    {
        LogResourceDesc("Committed", desc);

        const HRESULT result = CreateCommittedResource_hook.stdcall<HRESULT>(self, heapProperties, heapFlags, desc,
            initialState, optimizedClearValue, riid, resource);

        LogAllocationFailure("CreateCommittedResource", desc, result);
        RegisterUploadBuffer(heapProperties, desc, resource);
        return result;
    }

    SafetyHookInline CreateCommittedResource1_hook {};
    SafetyHookInline CreateCommittedResource2_hook {};

    HRESULT STDMETHODCALLTYPE Hooked_CreateCommittedResource1(ID3D12Device* self,
        const D3D12_HEAP_PROPERTIES* heapProperties, D3D12_HEAP_FLAGS heapFlags,
        const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
        const D3D12_CLEAR_VALUE* optimizedClearValue, void* protectedSession,
        REFIID riid, void** resource)
    {
        const HRESULT result = CreateCommittedResource1_hook.stdcall<HRESULT>(self, heapProperties,
            heapFlags, desc, initialState, optimizedClearValue, protectedSession, riid, resource);

        RegisterUploadBuffer(heapProperties, desc, resource);
        return result;
    }

    // Takes a D3D12_RESOURCE_DESC1, which begins with the same fields as D3D12_RESOURCE_DESC -
    // Dimension and Width are all this needs, and they sit at the same offsets.
    HRESULT STDMETHODCALLTYPE Hooked_CreateCommittedResource2(ID3D12Device* self,
        const D3D12_HEAP_PROPERTIES* heapProperties, D3D12_HEAP_FLAGS heapFlags,
        const void* desc1, D3D12_RESOURCE_STATES initialState,
        const D3D12_CLEAR_VALUE* optimizedClearValue, void* protectedSession,
        REFIID riid, void** resource)
    {
        const HRESULT result = CreateCommittedResource2_hook.stdcall<HRESULT>(self, heapProperties,
            heapFlags, desc1, initialState, optimizedClearValue, protectedSession, riid, resource);

        RegisterUploadBuffer(heapProperties,
            reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc1), resource);
        return result;
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreatePlacedResource(ID3D12Device* self,
        ID3D12Heap* heap, UINT64 heapOffset,
        const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES initialState,
        const D3D12_CLEAR_VALUE* optimizedClearValue, REFIID riid, void** resource)
    {
        LogResourceDesc("Placed", desc);

        const HRESULT result = CreatePlacedResource_hook.stdcall<HRESULT>(self, heap, heapOffset, desc,
            initialState, optimizedClearValue, riid, resource);

        LogAllocationFailure("CreatePlacedResource", desc, result);
        RegisterUploadBuffer(nullptr, desc, resource);
        return result;
    }

    // ------------- viewport / scissor logging -------------
    // The reticle's draw calls are issued at every render resolution but stop producing
    // output above a 4095-wide render buffer. Since the draws are present, the suspect is
    // the pass state they run under, so log what the engine actually sets.

    SafetyHookInline CreateCommandList_hook {};
    SafetyHookInline RSSetViewports_hook {};
    SafetyHookInline RSSetScissorRects_hook {};

    // ID3D12Device / ID3D12GraphicsCommandList vtable slots (stable COM ABI).
    constexpr size_t kCreateCommandListSlot = 12;
    constexpr size_t kRSSetViewportsSlot = 21;
    constexpr size_t kRSSetScissorRectsSlot = 22;

    // Logging is armed by a hotkey rather than running from startup: the interesting frame
    // is the one where the player is aiming, which is thousands of frames in. A fixed
    // budget from process start only ever captures the loading screens.
    std::atomic<int> g_StateLogBudget { 0 };
    std::atomic<int> g_StateLogSeq { 0 };

    bool TakeStateLogBudget()
    {
        int current = g_StateLogBudget.load(std::memory_order_relaxed);
        while (current > 0)
        {
            if (g_StateLogBudget.compare_exchange_weak(current, current - 1, std::memory_order_relaxed))
            {
                return true;
            }
        }
        return false;
    }

    void STDMETHODCALLTYPE Hooked_RSSetViewports(ID3D12GraphicsCommandList* self,
        UINT count, const D3D12_VIEWPORT* viewports)
    {
        if (viewports)
        {
            for (UINT i = 0; i < count && TakeStateLogBudget(); i++)
            {
                spdlog::info("MGS4: [{}] Viewport {}x{} at ({},{})",
                    g_StateLogSeq.fetch_add(1),
                    viewports[i].Width, viewports[i].Height,
                    viewports[i].TopLeftX, viewports[i].TopLeftY);
            }
        }

        RSSetViewports_hook.stdcall<void>(self, count, viewports);
    }

    void STDMETHODCALLTYPE Hooked_RSSetScissorRects(ID3D12GraphicsCommandList* self,
        UINT count, const D3D12_RECT* rects)
    {
        if (rects)
        {
            for (UINT i = 0; i < count && TakeStateLogBudget(); i++)
            {
                const bool degenerate = rects[i].right <= rects[i].left || rects[i].bottom <= rects[i].top;

                spdlog::info("MGS4: [{}] Scissor L={} T={} R={} B={}{}",
                    g_StateLogSeq.fetch_add(1),
                    rects[i].left, rects[i].top, rects[i].right, rects[i].bottom,
                    degenerate ? "   <-- EMPTY, clips everything" : "");
            }
        }

        RSSetScissorRects_hook.stdcall<void>(self, count, rects);
    }

    // Upload-heap buffers are CPU-visible, so mapping them once at creation lets us read
    // back whatever the engine writes into them - including the vertex positions a draw is
    // about to consume. Keyed by GPU virtual address so a vertex buffer view can be matched
    // back to its CPU pointer.
    struct MappedBuffer
    {
        D3D12_GPU_VIRTUAL_ADDRESS gpuAddress;
        const uint8_t* cpuAddress;
        UINT64 size;
    };

    std::vector<MappedBuffer> g_MappedBuffers;
    std::mutex g_MappedBuffersMutex;

    void RegisterUploadBuffer(const D3D12_HEAP_PROPERTIES* heapProperties,
        const D3D12_RESOURCE_DESC* desc, void** resource)
    {
        if ((!RenderPipeline::bLogViewports && !RenderPipeline::bGuardUploadBuffers)
            || !desc || !resource || !*resource
            || desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        {
            return;
        }

        // Identify the large buffers and which heap they live in. The UI vertex pool is 3MB
        // with stride 12, and whether it is UPLOAD (CPU-written directly) or DEFAULT (filled
        // by copy) decides where its contents can possibly come from - a question every probe
        // so far has been guessing at.
        if (desc->Width >= (1u << 20))
        {
            static std::atomic<int> reported { 0 };
            if (reported.fetch_add(1) < 12)
            {
                const char* heap = "placed/unknown";
                if (heapProperties)
                {
                    switch (heapProperties->Type)
                    {
                    case D3D12_HEAP_TYPE_DEFAULT:  heap = "DEFAULT (GPU only)"; break;
                    case D3D12_HEAP_TYPE_UPLOAD:   heap = "UPLOAD (CPU writable)"; break;
                    case D3D12_HEAP_TYPE_READBACK: heap = "READBACK"; break;
                    default: heap = "CUSTOM"; break;
                    }
                }

                const char* page = "n/a";
                if (heapProperties)
                {
                    switch (heapProperties->CPUPageProperty)
                    {
                    case D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE: page = "not CPU accessible"; break;
                    case D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE: page = "CPU write-combine"; break;
                    case D3D12_CPU_PAGE_PROPERTY_WRITE_BACK:    page = "CPU write-back"; break;
                    default: page = "unknown"; break;
                    }
                }

                spdlog::info("MGS4: Buffer created: {} bytes in {}, {}", desc->Width, heap, page);
            }
        }

        // heapProperties is null for placed resources; in that case just attempt the map
        // and let it fail harmlessly if the heap isn't CPU-visible.
        //
        // This engine allocates everything from D3D12_HEAP_TYPE_CUSTOM, so testing for
        // HEAP_TYPE_UPLOAD rejected every buffer it has - which is why nothing was ever
        // mapped, every vertex dump said "not in a mapped buffer", and the staging search
        // found nothing. With a custom heap it is CPUPageProperty that says whether the CPU
        // can write it, not the type.
        if (heapProperties
            && heapProperties->Type != D3D12_HEAP_TYPE_UPLOAD
            && !(heapProperties->Type == D3D12_HEAP_TYPE_CUSTOM
                && heapProperties->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_UNKNOWN
                && heapProperties->CPUPageProperty != D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE))
        {
            return;
        }

        auto* buffer = static_cast<ID3D12Resource*>(*resource);
        void* mapped = nullptr;

        // Upload heaps may stay persistently mapped, so this is safe to leave in place.
        const HRESULT mapResult = buffer->Map(0, nullptr, &mapped);

        if (desc->Width >= (1u << 20))
        {
            static std::atomic<int> reported { 0 };
            if (reported.fetch_add(1) < 48)
            {
                spdlog::info("MGS4: Buffer map: {} bytes at gpu 0x{:X} -> {}",
                    desc->Width, buffer->GetGPUVirtualAddress(),
                    SUCCEEDED(mapResult) && mapped
                        ? std::format("cpu 0x{:X}", reinterpret_cast<uintptr_t>(mapped))
                        : std::format("FAILED (0x{:08X})", static_cast<uint32_t>(mapResult)));
            }
        }

        if (FAILED(mapResult) || !mapped)
        {
            return;
        }

        std::lock_guard lock(g_MappedBuffersMutex);
        g_MappedBuffers.push_back({ buffer->GetGPUVirtualAddress(), static_cast<const uint8_t*>(mapped), desc->Width });

        // Guard the buffer the moment it exists, before anything has written to it. This is
        // the one arrangement that can catch the conversion: the coordinate is written once,
        // and by the time its address can be found the write is long past. Guarding a single
        // buffer is also safe, unlike guarding every heap, which wedged the process - the CPU
        // faults once here, we note who did it, and the guard comes straight off.
        if (RenderPipeline::bGuardUploadBuffers)
        {
            RegisterUploadGuard(reinterpret_cast<uintptr_t>(mapped), static_cast<size_t>(desc->Width));
        }
    }

    // Resolves a vertex buffer view back to the CPU-side bytes it points at.
    const uint8_t* ResolveVertexData(D3D12_GPU_VIRTUAL_ADDRESS address, UINT64 needed)
    {
        std::lock_guard lock(g_MappedBuffersMutex);
        for (const auto& buffer : g_MappedBuffers)
        {
            if (address >= buffer.gpuAddress && (address + needed) <= (buffer.gpuAddress + buffer.size))
            {
                return buffer.cpuAddress + (address - buffer.gpuAddress);
            }
        }
        return nullptr;
    }

    // Identifying which vertex shader draws the reticle: DXBC blobs carry a 16-byte checksum
    // right after the magic, and the community shader dumps are named after it (as
    // byte-swapped dwords), so hashing what the game loads maps a draw back to a source file.
    SafetyHookInline CreateGraphicsPipelineState_hook {};
    SafetyHookInline SetPipelineState_hook {};
    constexpr size_t kCreateGraphicsPipelineStateSlot = 10;
    constexpr size_t kSetPipelineStateSlot = 25;

    std::mutex g_PipelineMutex;
    std::unordered_map<void*, std::string> g_PipelineVertexShader;
    thread_local void* g_CurrentPipeline = nullptr;

    // Which input layout each pipeline was built with. Two distinct layouts exist for the
    // reticle shader and only one of them can be the one that actually renders it, so a draw
    // has to be able to say which it is using.
    std::map<void*, int> g_PipelineLayoutId;
    thread_local int g_PendingLayoutId = 0;

    // Where v6, v7 and v8 actually live for a given pipeline, taken from its input layout
    // rather than assumed. Reading v8 from a hardcoded offset 80 is only right when the
    // pipeline happens to place it there; others came back shifted by whole float4s, with v6
    // reading as (1,1,1) as the tell. The layout is already in hand at creation, so the read
    // can simply be derived.
    struct AnchorLocation
    {
        bool valid = false;
        UINT slot = 0;
        UINT offsetV6 = 0;
        UINT offsetV7 = 0;
        UINT offsetV8 = 0;
    };

    std::map<void*, AnchorLocation> g_PipelineAnchor;
    thread_local AnchorLocation g_PendingAnchor {};

    AnchorLocation CurrentPipelineAnchor()
    {
        std::lock_guard lock(g_PipelineMutex);
        const auto it = g_PipelineAnchor.find(g_CurrentPipeline);
        return it == g_PipelineAnchor.end() ? AnchorLocation {} : it->second;
    }

    int CurrentPipelineLayoutId()
    {
        std::lock_guard lock(g_PipelineMutex);
        const auto it = g_PipelineLayoutId.find(g_CurrentPipeline);
        return it == g_PipelineLayoutId.end() ? 0 : it->second;
    }

    std::string DescribeShaderChecksum(const void* bytecode, size_t size)
    {
        if (!bytecode || size < 20)
        {
            return {};
        }

        const auto* bytes = static_cast<const uint8_t*>(bytecode);
        if (std::memcmp(bytes, "DXBC", 4) != 0)
        {
            return {};
        }

        // Printed as four byte-swapped dwords to match the shader dump filenames.
        std::string result;
        for (int dword = 0; dword < 4; dword++)
        {
            if (dword > 0)
            {
                result += '-';
            }
            for (int byte = 3; byte >= 0; byte--)
            {
                result += std::format("{:02x}", bytes[4 + (dword * 4) + byte]);
            }
        }
        return result;
    }

    // Identifies the UI vertex shader that draws the aiming reticle. The pipeline-to-shader
    // map below uses it to attribute draws while investigating UI coordinates.
    //
    // This used to be accompanied by a patched copy of the shader, substituted at pipeline
    // creation to undo the 16-bit position wrap. That workaround is gone: the truncation is
    // fixed at its source in graphics_settings.cpp, so there is no wrap left to undo.
    constexpr const char* kReticleVertexShaderChecksum = "c0f6fdb2-73bd444d-069170c9-a85fc8a1";

    // Raises MaxAnisotropy on samplers that already ask for anisotropic filtering.
    SafetyHookInline CreateSampler_hook {};

    void STDMETHODCALLTYPE Hooked_CreateSampler(ID3D12Device* self,
        const D3D12_SAMPLER_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE destination)
    {
        const int wanted = RenderPipeline::iAnisotropicFiltering;

        if (desc && wanted > 0 && desc->MaxAnisotropy > 0
            && desc->MaxAnisotropy < static_cast<UINT>(wanted)
            && (desc->Filter == D3D12_FILTER_ANISOTROPIC
                || desc->Filter == D3D12_FILTER_COMPARISON_ANISOTROPIC
                || desc->Filter == D3D12_FILTER_MINIMUM_ANISOTROPIC
                || desc->Filter == D3D12_FILTER_MAXIMUM_ANISOTROPIC))
        {
            D3D12_SAMPLER_DESC raised = *desc;
            raised.MaxAnisotropy = static_cast<UINT>(wanted);

            static std::atomic<int> reported { 0 };
            if (reported.fetch_add(1) < 3)
            {
                spdlog::info("MGS4: Anisotropic Filtering: sampler {}x -> {}x.",
                    desc->MaxAnisotropy, wanted);
            }

            CreateSampler_hook.stdcall<void>(self, &raised, destination);
            return;
        }

        CreateSampler_hook.stdcall<void>(self, desc, destination);
    }

    // The same for D3D11, the renderer the game ships set to: MaxAnisotropy on CreateSamplerState.
    SafetyHookInline CreateSamplerState_hook {};

    HRESULT STDMETHODCALLTYPE Hooked_CreateSamplerState(ID3D11Device* self,
        const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** sampler)
    {
        const int wanted = RenderPipeline::iAnisotropicFiltering;

        if (desc && wanted > 0 && desc->MaxAnisotropy > 0
            && desc->MaxAnisotropy < static_cast<UINT>(wanted)
            && (desc->Filter == D3D11_FILTER_ANISOTROPIC
                || desc->Filter == D3D11_FILTER_COMPARISON_ANISOTROPIC
                || desc->Filter == D3D11_FILTER_MINIMUM_ANISOTROPIC
                || desc->Filter == D3D11_FILTER_MAXIMUM_ANISOTROPIC))
        {
            D3D11_SAMPLER_DESC raised = *desc;
            raised.MaxAnisotropy = static_cast<UINT>(wanted);

            static std::atomic<int> reported { 0 };
            if (reported.fetch_add(1) < 3)
            {
                spdlog::info("MGS4: Anisotropic Filtering: D3D11 sampler {}x -> {}x.",
                    desc->MaxAnisotropy, wanted);
            }

            return CreateSamplerState_hook.stdcall<HRESULT>(self, &raised, sampler);
        }

        return CreateSamplerState_hook.stdcall<HRESULT>(self, desc, sampler);
    }

    // Rough size of an input element, for advancing D3D12_APPEND_ALIGNED_ELEMENT offsets.
    // Only the formats the game actually uses need to be exact; anything unknown gets 16,
    // which can only over-advance - and an over-advanced running offset can never produce
    // the false six-float4 match below, only miss one.
    UINT InputFormatSize(DXGI_FORMAT format)
    {
        switch (format)
        {
            case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
            case DXGI_FORMAT_R32G32B32_FLOAT: return 12;
            case DXGI_FORMAT_R32G32_FLOAT: return 8;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_SINT:
            case DXGI_FORMAT_R16G16B16A16_SNORM: return 8;
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_R32_UINT:
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UINT:
            case DXGI_FORMAT_R16G16_FLOAT:
            case DXGI_FORMAT_R16G16_SINT:
            case DXGI_FORMAT_R16G16_SNORM: return 4;
            default: return 16;
        }
    }

    // Finds the UI's per-draw constant stream in a pipeline's input layout: six consecutive
    // float4s on one slot, at offsets 0,16,..,80. Within it, v6/v7 are the rotation basis at
    // 48/64 and v8 the NDC anchor at 80 - the layout the UI research measured from live vertex
    // decode. Nothing here trusts a shader checksum: any pipeline with that stream shape gets
    // an anchor location, and the plausibility check at the read site (v6/v7 axis-aligned,
    // v8.z == 1) is what confirms the interpretation per draw.
    //
    // This is what populates g_PendingAnchor. It has to exist for ANCHOR sampling to work at
    // all: the original populating analysis lived in the removed shader-workaround path, and
    // without it the sampler read an empty map forever - 24000 draw samples, zero anchors, no
    // error. The consumer in Hooked_CreateGraphicsPipelineState was checking a flag nothing set.
    AnchorLocation FindAnchorStream(const D3D12_INPUT_LAYOUT_DESC& layout)
    {
        constexpr UINT kMaxSlots = 16;
        UINT running[kMaxSlots] = {};
        uint32_t float4Mask[kMaxSlots] = {};

        for (UINT i = 0; i < layout.NumElements; i++)
        {
            const auto& element = layout.pInputElementDescs[i];
            if (element.InputSlot >= kMaxSlots)
            {
                continue;
            }

            UINT offset = element.AlignedByteOffset;
            if (offset == D3D12_APPEND_ALIGNED_ELEMENT)
            {
                offset = running[element.InputSlot];
            }
            running[element.InputSlot] = offset + InputFormatSize(element.Format);

            if (element.Format == DXGI_FORMAT_R32G32B32A32_FLOAT
                && offset % 16 == 0 && offset / 16 < 16)
            {
                float4Mask[element.InputSlot] |= 1u << (offset / 16);
            }
        }

        // Any run of six consecutive float4s qualifies, wherever it starts - an older probe
        // that assumed offsets 48/64/80 got anchors shifted by whole float4s on pipelines
        // that put other elements first, with v6 reading as (1,1,1) as the giveaway.
        for (UINT slot = 0; slot < kMaxSlots; slot++)
        {
            for (UINT start = 0; start + 6 <= 16; start++)
            {
                if ((float4Mask[slot] >> start & 0x3F) == 0x3F)
                {
                    const UINT base = start * 16;
                    return { true, slot, base + 48, base + 64, base + 80 };
                }
            }
        }
        return {};
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateGraphicsPipelineState(ID3D12Device* self,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** pipelineState)
    {
        std::string checksum;
        if (desc)
        {
            checksum = DescribeShaderChecksum(desc->VS.pShaderBytecode, desc->VS.BytecodeLength);
            if (desc->InputLayout.pInputElementDescs && desc->InputLayout.NumElements > 0)
            {
                g_PendingAnchor = FindAnchorStream(desc->InputLayout);
            }
        }

        const HRESULT result = CreateGraphicsPipelineState_hook.stdcall<HRESULT>(self, desc, riid, pipelineState);

        if (SUCCEEDED(result) && !checksum.empty() && pipelineState && *pipelineState)
        {
            std::lock_guard lock(g_PipelineMutex);
            g_PipelineVertexShader[*pipelineState] = std::move(checksum);
            if (g_PendingLayoutId != 0)
            {
                g_PipelineLayoutId[*pipelineState] = g_PendingLayoutId;
            }
            if (g_PendingAnchor.valid)
            {
                g_PipelineAnchor[*pipelineState] = g_PendingAnchor;
            }
        }
        g_PendingLayoutId = 0;
        g_PendingAnchor = {};

        return result;
    }

    void STDMETHODCALLTYPE Hooked_SetPipelineState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pipeline)
    {
        g_CurrentPipeline = pipeline;
        SetPipelineState_hook.stdcall<void>(self, pipeline);
    }

    std::string CurrentVertexShaderChecksum()
    {
        std::lock_guard lock(g_PipelineMutex);
        const auto it = g_PipelineVertexShader.find(g_CurrentPipeline);
        return it == g_PipelineVertexShader.end() ? std::string { "<unknown>" } : it->second;
    }

    SafetyHookInline DrawInstanced_hook {};
    SafetyHookInline IASetVertexBuffers_hook {};
    SafetyHookInline SetGraphicsRootCBV_hook {};
    constexpr size_t kDrawInstancedSlot = 12;
    constexpr size_t kSetGraphicsRootCBVSlot = 38;
    constexpr size_t kIASetVertexBuffersSlot = 44;

    // The per-draw constant buffer is where a UI transform would live, and dynamic constant
    // buffers are almost always upload-heap, so this is readable where the vertex data was not.
    //
    // Kept per root parameter rather than as a single "last one set". A draw binds several
    // buffers at different root slots, so collapsing them to one variable meant the dump was
    // showing whichever happened to be bound last - not necessarily the one carrying the
    // transform, which is why it read as repeating filler.
    constexpr UINT kMaxRootParameters = 16;
    thread_local D3D12_GPU_VIRTUAL_ADDRESS g_RootCBV[kMaxRootParameters] {};
    thread_local D3D12_GPU_VIRTUAL_ADDRESS g_LastRootCBV = 0;

    void STDMETHODCALLTYPE Hooked_SetGraphicsRootCBV(ID3D12GraphicsCommandList* self,
        UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
    {
        if (rootParameterIndex < kMaxRootParameters)
        {
            g_RootCBV[rootParameterIndex] = address;
        }
        g_LastRootCBV = address;
        SetGraphicsRootCBV_hook.stdcall<void>(self, rootParameterIndex, address);
    }

    // Root 32-bit constants: values written straight into the root signature rather than into
    // a buffer, which is exactly how an engine passes a small per-draw transform. Nothing was
    // watching these, so if the reticle's translate is here it has been invisible all along.
    SafetyHookInline SetGraphicsRoot32BitConstants_hook {};
    SafetyHookInline SetGraphicsRoot32BitConstant_hook {};
    constexpr size_t kSetGraphicsRoot32BitConstantSlot = 34;
    constexpr size_t kSetGraphicsRoot32BitConstantsSlot = 36;

    struct RootConstantRecord
    {
        UINT count;
        uint32_t values[64];
    };

    thread_local RootConstantRecord g_RootConstants[kMaxRootParameters] {};

    void STDMETHODCALLTYPE Hooked_SetGraphicsRoot32BitConstants(ID3D12GraphicsCommandList* self,
        UINT rootParameterIndex, UINT num32BitValuesToSet, const void* data,
        UINT destOffsetIn32BitValues)
    {
        if (rootParameterIndex < kMaxRootParameters && data)
        {
            RootConstantRecord& record = g_RootConstants[rootParameterIndex];
            for (UINT i = 0; i < num32BitValuesToSet; i++)
            {
                const UINT slot = destOffsetIn32BitValues + i;
                if (slot < 64)
                {
                    record.values[slot] = static_cast<const uint32_t*>(data)[i];
                    if (slot + 1 > record.count)
                    {
                        record.count = slot + 1;
                    }
                }
            }
        }

        SetGraphicsRoot32BitConstants_hook.stdcall<void>(self, rootParameterIndex,
            num32BitValuesToSet, data, destOffsetIn32BitValues);
    }

    void STDMETHODCALLTYPE Hooked_SetGraphicsRoot32BitConstant(ID3D12GraphicsCommandList* self,
        UINT rootParameterIndex, UINT srcData, UINT destOffsetIn32BitValues)
    {
        if (rootParameterIndex < kMaxRootParameters && destOffsetIn32BitValues < 64)
        {
            RootConstantRecord& record = g_RootConstants[rootParameterIndex];
            record.values[destOffsetIn32BitValues] = srcData;
            if (destOffsetIn32BitValues + 1 > record.count)
            {
                record.count = destOffsetIn32BitValues + 1;
            }
        }

        SetGraphicsRoot32BitConstant_hook.stdcall<void>(self, rootParameterIndex, srcData,
            destOffsetIn32BitValues);
    }


    // Remembers the most recent vertex buffer view per command list so a draw can report
    // the geometry it is about to consume.
    thread_local D3D12_VERTEX_BUFFER_VIEW g_LastVertexBuffer {};

    // The UI binds five streams at once (mgs4.exe+79F40B passes NumViews=5), and the shader
    // takes its int16 position from one and its float anchor from another. Keeping all of them
    // is what makes it possible to see both halves of a vertex at the same time.
    thread_local D3D12_VERTEX_BUFFER_VIEW g_LastVertexStreams[8] {};
    thread_local UINT g_LastVertexStreamCount = 0;

    void STDMETHODCALLTYPE Hooked_IASetVertexBuffers(ID3D12GraphicsCommandList* self,
        UINT startSlot, UINT numViews, const D3D12_VERTEX_BUFFER_VIEW* views)
    {
        if (views && numViews > 0)
        {
            // Record each view at the slot it is actually bound to. The previous version wrote
            // views[i] to stream[i] regardless of startSlot, so a call binding slots 1-4 landed
            // in 0-3, and a second call clobbered the first rather than adding to it. Bindings
            // persist on the command list, so they have to accumulate here the same way.
            for (UINT i = 0; i < numViews; i++)
            {
                const UINT slot = startSlot + i;
                if (slot >= 8)
                {
                    break;
                }

                g_LastVertexStreams[slot] = views[i];
                if (slot + 1 > g_LastVertexStreamCount)
                {
                    g_LastVertexStreamCount = slot + 1;
                }
            }

            if (startSlot == 0)
            {
                g_LastVertexBuffer = views[0];
            }

            // Catch the binding of the reticle's per-quad stream directly.
            //
            // Layout #2 puts v3..v8 on slot 1 as six float4s, so that buffer has a 96-byte
            // stride. No draw we have hooked ever had it bound - the streams=5 counts turned
            // out to be null views inflating the high-water mark - so find the bind instead of
            // the draw. The call stack here is the engine code that submits the reticle.
            // Every distinct binding shape, once each.
            //
            // Layout #2 is bound on ~99% of sampled UI draws and declares a slot 1, yet no draw
            // has ever had slot 1 bound - only slot 0 at stride 12. D3D12 feeds zeros for an
            // unbound slot, which would collapse those quads to the origin, so either the
            // binding happens somewhere this hook does not see or it never happens at all.
            // Logging the shapes distinguishes the two.
            if (RenderPipeline::bLogViewports)
            {
                for (UINT i = 0; i < numViews; i++)
                {
                    const std::string shape = std::format("slot{} n{} stride{} {}",
                        startSlot + i, numViews, views[i].StrideInBytes,
                        views[i].BufferLocation != 0 ? "bound" : "NULL");

                    static std::set<std::string> seenShapes;
                    static std::mutex shapeMutex;
                    bool isNew = false;
                    {
                        std::lock_guard lock(shapeMutex);
                        if (seenShapes.size() < 60)
                        {
                            isNew = seenShapes.insert(shape).second;
                        }
                    }

                    if (isNew)
                    {
                        spdlog::info("VBSHAPE {}{}", shape,
                            (startSlot + i) >= 1 ? DescribeGameCallers() : "");
                    }
                }
            }
        }

        IASetVertexBuffers_hook.stdcall<void>(self, startSlot, numViews, views);
    }

    // Whether the 3MB UI vertex pool is filled by GPU copy, and from where. If it is, the
    // source is staging memory the CPU does write - which is where the coordinates are
    // assembled. If nothing ever copies into it, then it is CPU-written after all and the
    // failure to resolve it was our own bookkeeping.
    std::atomic<bool> g_StagingWatchArmed { false };

    // Recent buffer copies, so a vertex address in the GPU-only pool can be traced back to the
    // CPU-visible memory it was copied from. Searching the copies for a guessed coordinate
    // failed because the reticle is not exactly centre-screen; computing the offset instead
    // needs no guess at all.
    struct RecentCopy
    {
        D3D12_GPU_VIRTUAL_ADDRESS dstStart;
        UINT64 bytes;
        ID3D12Resource* src;
        UINT64 srcOffset;
    };

    std::array<RecentCopy, 64> g_RecentCopies {};
    std::atomic<size_t> g_RecentCopyIndex { 0 };
    std::mutex g_RecentCopyMutex;

    // Set while the aim button is held, so sampling happens on the crosshair rather than on
    // whatever the menus draw at startup.
    std::atomic<bool> g_AimHeld { false };
    std::atomic<int> g_AimSampleBudget { 0 };

    // The matching sample taken just after aiming stops, for the aiming/idle differential.
    std::atomic<int> g_IdleSampleBudget { 0 };

    // Indexed draws get their own budgets. Sharing the ones above meant the couple of thousand
    // DrawInstanced calls drained them before a single indexed draw was reached - and the
    // indexed draws are the ones that matter, since that is where the reticle is.
    // Two budgets, because scanning and logging want opposite things.
    //
    // The wrapped anchor is rare - 7 of 1600 samples in one run, 0 in the next - so gating the
    // *scan* on a budget means unrelated UI quads spend it before the reticle is reached, and
    // the run comes back empty. The scan budget is therefore large; a separate, small logging
    // budget keeps the log readable. A wrapped anchor always logs and always arms, regardless
    // of either.
    std::atomic<int> g_AnchorBudget { 0 };
    std::atomic<int> g_AnchorLogBudget { 0 };
    std::atomic<int> g_IndexedAimBudget { 0 };
    std::atomic<int> g_IndexedIdleBudget { 0 };

    void StartAimWatchThread()
    {
        std::thread([]
            {
                bool wasDown = false;
                while (true)
                {
                    const bool down = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

                    // Fresh budget each time aiming starts, so every press gives a sample.
                    //
                    // A frame of UI is roughly 400 quad draws, and 8 only ever caught the first
                    // few - which are the full-screen backdrops, never the reticle. Now that
                    // each draw costs one summary line, several whole frames fit comfortably
                    // inside the log cap, so take enough to be sure the reticle is among them.
                    if (down && !wasDown)
                    {
                        g_AimSampleBudget.store(2000);
                        g_IndexedAimBudget.store(6000);
                        g_AnchorBudget.store(40000);
                        g_AnchorLogBudget.store(400);
                    }

                    // And an equal sample the moment aiming stops. The reticle is the one UI
                    // element that exists exactly while the aim button is held, so a sample
                    // taken immediately after release - same scene, same camera, same frame
                    // budget - differs from the aiming sample by the reticle and very little
                    // else. Diffing the two identifies its draw without having to guess at
                    // size, position or call site.
                    if (!down && wasDown)
                    {
                        g_IdleSampleBudget.store(2000);
                        g_IndexedIdleBudget.store(6000);
                        g_AnchorBudget.store(40000);
                        g_AnchorLogBudget.store(400);
                    }

                    g_AimHeld.store(down);
                    wasDown = down;
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            }).detach();
    }

    // Defined after the breakpoint machinery it uses.
    void TryArmStagingWatch(const uint8_t* data, UINT64 bytes);

    // Watch one known address, rather than searching a buffer for a value.
    //
    // TryArmStagingWatch hunts for the wrapped coordinate as an int16 pair, which is why it
    // never found anything: v8 is a float in NDC by the time it reaches the vertex buffer, and
    // the int16 truncation happens further upstream. But the layout now tells us exactly where
    // v8 lives, so the address can be watched directly and whatever writes it will announce
    // itself.

    // Every narrowing of a converted coordinate to 16 bits, with what it is scaled by.
    //
    // The shipped fix widens only those it can prove are screen coordinates, by requiring a
    // division by render.bufferSizeX or bufferSizeY. That constraint is what makes patching safe,
    // but it also hides any narrowing reached another way - a fixed 1280x720 assumption, a
    // different global, or nothing at all. This reports the lot so the remainder can be judged
    // rather than assumed absent.
    //
    // Read-only: it never writes to the image.
    void ScanNarrowingConversions()
    {
        const auto imageBase = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mgs4e::game::Module());
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const uint8_t*>(mgs4e::game::Module()) + dos->e_lfanew);

        uint8_t* textStart = nullptr;
        size_t textSize = 0;
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++)
        {
            if (std::memcmp(section->Name, ".text", 5) == 0)
            {
                textStart = reinterpret_cast<uint8_t*>(imageBase) + section->VirtualAddress;
                textSize = section->Misc.VirtualSize;
                break;
            }
        }

        if (!textStart)
        {
            spdlog::error("MGS4: Narrowing scan: could not locate .text.");
            return;
        }

        const size_t textRva =
            static_cast<size_t>(textStart - reinterpret_cast<uint8_t*>(imageBase));
        const uintptr_t widthGlobal = imageBase + kRvaRenderBufferSizeX;
        const uintptr_t heightGlobal = imageBase + kRvaRenderBufferSizeY;
        const uintptr_t windowWidthGlobal = imageBase + kRvaRenderWindowSizeX;
        const uintptr_t windowHeightGlobal = imageBase + kRvaRenderWindowSizeY;

        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

        constexpr std::array<uint8_t, 4> kConvert = { 0xF3, 0x0F, 0x2C, 0xC8 };

        int total = 0;
        int scaled = 0;

        spdlog::info("MGS4: Narrowing scan: looking for 16-bit narrowings of converted values.");

        for (size_t at = 0; at + 64 < textSize; at++)
        {
            if (std::memcmp(textStart + at, kConvert.data(), kConvert.size()) != 0)
            {
                continue;
            }

            size_t offset = at + kConvert.size();
            size_t movsxOffset = 0;
            ZydisRegister destination = ZYDIS_REGISTER_NONE;
            std::string scaleNote;

            for (int step = 0; step < 12 && offset + 16 < textSize; step++)
            {
                ZydisDecodedInstruction instruction;
                ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, textStart + offset,
                    textSize - offset, &instruction, operands)))
                {
                    break;
                }

                if (movsxOffset == 0
                    && instruction.mnemonic == ZYDIS_MNEMONIC_MOVSX
                    && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER
                    && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER
                    && operands[1].reg.value == ZYDIS_REGISTER_CX)
                {
                    movsxOffset = offset;
                    destination = operands[0].reg.value;
                }
                else if (movsxOffset != 0)
                {
                    // An immediate multiply says which coordinate space it is heading for.
                    if (instruction.mnemonic == ZYDIS_MNEMONIC_IMUL)
                    {
                        for (ZyanU8 i = 0; i < instruction.operand_count; i++)
                        {
                            if (operands[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
                            {
                                scaleNote += std::format(" imul {}",
                                    operands[i].imm.value.s);
                            }
                        }
                    }

                    for (ZyanU8 i = 0; i < instruction.operand_count; i++)
                    {
                        if (operands[i].type != ZYDIS_OPERAND_TYPE_MEMORY
                            || operands[i].mem.base != ZYDIS_REGISTER_RIP)
                        {
                            continue;
                        }

                        const auto target = reinterpret_cast<uintptr_t>(textStart + offset)
                            + instruction.length
                            + static_cast<uintptr_t>(operands[i].mem.disp.value);

                        if (target == widthGlobal)       { scaleNote += " /bufferW"; }
                        else if (target == heightGlobal) { scaleNote += " /bufferH"; }
                        else if (target == windowWidthGlobal)  { scaleNote += " /windowW"; }
                        else if (target == windowHeightGlobal) { scaleNote += " /windowH"; }
                    }
                }

                offset += instruction.length;
            }

            if (movsxOffset == 0)
            {
                continue;
            }

            total++;
            const bool isScreenSpace = scaleNote.find("/buffer") != std::string::npos;
            if (isScreenSpace)
            {
                scaled++;
            }

            spdlog::info("MGS4: Narrowing +{:X}  movsx {}, cx {}{}",
                textRva + movsxOffset, ZydisRegisterGetString(destination),
                scaleNote.empty() ? "(no render-size scale found)" : scaleNote,
                isScreenSpace ? "   <-- patched by the reticle fix" : "");
        }

        spdlog::info("MGS4: Narrowing scan: {} narrowing(s) total, {} of them scaled by a render "
            "buffer size. The remaining {} are reached another way and are NOT patched.",
            total, scaled, total - scaled);
    }

    void ScanUiInstanceArray();
    void ScanProcessForWrappedPair();

    void ArmAnchorWatch(uintptr_t address, const char* what);
    SafetyHookInline CopyBufferRegion_hook {};
    constexpr size_t kCopyBufferRegionSlot = 15;

    void STDMETHODCALLTYPE Hooked_CopyBufferRegion(ID3D12GraphicsCommandList* self,
        ID3D12Resource* dst, UINT64 dstOffset, ID3D12Resource* src, UINT64 srcOffset, UINT64 bytes)
    {
        if (RenderPipeline::bLogViewports && g_StateLogBudget.load() > 0)
        {
            static std::atomic<int> reported { 0 };
            if (reported.fetch_add(1) < 24 && dst && src)
            {
                spdlog::info("MGS4: CopyBufferRegion  dst 0x{:X}+{}  <- src 0x{:X}+{}  ({} bytes)",
                    dst->GetGPUVirtualAddress(), dstOffset,
                    src->GetGPUVirtualAddress(), srcOffset, bytes);
            }
        }

        // The staging buffer is CPU-written every frame, unlike the vertex pool which is
        // written once - so a watch placed on it will actually fire.
        if (RenderPipeline::bWatchStagingWrites && !g_StagingWatchArmed.load()
            && src && bytes >= 4 && bytes < (8u << 20))
        {
            // Map the source directly rather than going through our own bookkeeping, which
            // never had this buffer - it is large and created through a path we do not track.
            // Map is reference-counted, so calling it on an already-mapped resource is fine.
            void* mapped = nullptr;
            if (SUCCEEDED(src->Map(0, nullptr, &mapped)) && mapped)
            {
                TryArmStagingWatch(static_cast<const uint8_t*>(mapped) + srcOffset, bytes);
            }
            else
            {
                static std::atomic<int> complained { 0 };
                if (complained.fetch_add(1) < 2)
                {
                    spdlog::warn("MGS4: Staging: could not map the copy source - it is not CPU-visible.");
                }
            }
        }

        if (RenderPipeline::bWatchStagingWrites && dst && src)
        {
            std::lock_guard lock(g_RecentCopyMutex);
            const size_t slot = g_RecentCopyIndex.fetch_add(1) % g_RecentCopies.size();
            g_RecentCopies[slot] = RecentCopy {
                dst->GetGPUVirtualAddress() + dstOffset, bytes, src, srcOffset };
        }

        CopyBufferRegion_hook.stdcall<void>(self, dst, dstOffset, src, srcOffset, bytes);
    }

    // Given an address in the GPU-only vertex pool, find the CPU memory it was copied from.
    const uint8_t* TraceVertexToStaging(D3D12_GPU_VIRTUAL_ADDRESS vertexAddress, UINT64 needed)
    {
        std::lock_guard lock(g_RecentCopyMutex);
        for (const auto& copy : g_RecentCopies)
        {
            if (copy.src == nullptr || copy.bytes == 0
                || vertexAddress < copy.dstStart
                || vertexAddress + needed > copy.dstStart + copy.bytes)
            {
                continue;
            }

            void* mapped = nullptr;
            if (FAILED(copy.src->Map(0, nullptr, &mapped)) || !mapped)
            {
                continue;
            }

            const UINT64 delta = vertexAddress - copy.dstStart;
            return static_cast<const uint8_t*>(mapped) + copy.srcOffset + delta;
        }
        return nullptr;
    }

    // Indexed draws, which is where the reticle actually is.
    //
    // The reticle vertex shader (c0f6fdb2) declares nine input registers:
    //
    //   v0 COLOR0     float4      per-vertex
    //   v1 POSITION0  int    xy   per-vertex   - a small local quad corner
    //   v2 TEXCOORD0  int    xy   per-vertex
    //   v3 TEXCOORD1  float4      \
    //   v4 TEXCOORD2  float4       |
    //   v5 TEXCOORD3  float4       |  per-instance
    //   v6 TEXCOORD4  float3       |
    //   v7 TEXCOORD5  float3       |
    //   v8 TEXCOORD7  float3      /
    //
    // and computes, with no perspective divide (o0.w is a literal 1.0):
    //
    //   clip.xyz = (POSITION.x * v3.x) * v6 + (POSITION.y * v3.y) * v7 + v8
    //
    // So v8 is the quad's centre already in normalised device coordinates, and v6/v7 are its
    // basis vectors. v0/v1/v2 are exactly the 12 bytes of the one stream we could see; v3..v8
    // have to come from a second, per-instance stream that was never bound on any draw we
    // captured. That is because those draws were not the reticle's: only DrawInstanced was
    // hooked, and the reticle is drawn indexed.
    SafetyHookInline DrawIndexedInstanced_hook {};
    constexpr size_t kDrawIndexedInstancedSlot = 13;

    void STDMETHODCALLTYPE Hooked_DrawIndexedInstanced(ID3D12GraphicsCommandList* self,
        UINT indexCountPerInstance, UINT instanceCount, UINT startIndexLocation,
        INT baseVertexLocation, UINT startInstanceLocation)
    {
        const bool aiming = g_AimHeld.load();
        if (RenderPipeline::bLogViewports
            && RenderPipeline::bWatchStagingWrites
            && ((aiming && g_IndexedAimBudget.fetch_sub(1) > 0)
                || (!aiming && g_IndexedIdleBudget.fetch_sub(1) > 0)))
        {
            spdlog::info("IDXDRAW {} vs={} idx={} inst={} startIdx={} baseVtx={} startInst={} "
                "streams={} site={}",
                aiming ? "AIM" : "IDLE",
                CurrentVertexShaderChecksum().substr(0, 8),
                indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation,
                startInstanceLocation, g_LastVertexStreamCount, UiDrawCallSite());

            // Every bound stream, but only for the reticle - five stream dumps on every
            // indexed draw in a frame would overrun the log cap long before the reticle
            // arrives, which is exactly how earlier captures lost their target.
            if (CurrentVertexShaderChecksum() != kReticleVertexShaderChecksum)
            {
                DrawIndexedInstanced_hook.stdcall<void>(self, indexCountPerInstance, instanceCount,
                    startIndexLocation, baseVertexLocation, startInstanceLocation);
                return;
            }

            for (UINT stream = 0; stream < g_LastVertexStreamCount; stream++)
            {
                const auto& view = g_LastVertexStreams[stream];
                if (view.BufferLocation == 0 || view.StrideInBytes == 0)
                {
                    continue;
                }

                const UINT64 offset =
                    static_cast<UINT64>(startInstanceLocation) * view.StrideInBytes;
                const uint8_t* data = ResolveVertexData(view.BufferLocation + offset,
                    view.StrideInBytes);
                if (!data)
                {
                    data = TraceVertexToStaging(view.BufferLocation + offset, view.StrideInBytes);
                }

                if (!data)
                {
                    spdlog::info("    stream {}: stride {} size {} <unresolvable>",
                        stream, view.StrideInBytes, view.SizeInBytes);
                    continue;
                }

                // Read as floats: the per-instance stream is float basis vectors and a float
                // translate, so a float view is the meaningful one here.
                std::string floats;
                for (UINT b = 0; b + 4 <= view.StrideInBytes && b < 96; b += 4)
                {
                    float value {};
                    std::memcpy(&value, data + b, sizeof(value));
                    floats += std::format(" {:.4f}", value);
                }

                spdlog::info("    stream {}: stride {} size {} floats:{}",
                    stream, view.StrideInBytes, view.SizeInBytes, floats);
            }
        }

        DrawIndexedInstanced_hook.stdcall<void>(self, indexCountPerInstance, instanceCount,
            startIndexLocation, baseVertexLocation, startInstanceLocation);
    }

    void STDMETHODCALLTYPE Hooked_DrawInstanced(ID3D12GraphicsCommandList* self,
        UINT vertexCountPerInstance, UINT instanceCount, UINT startVertexLocation, UINT startInstanceLocation)
    {
        // With buffers finally being mapped, the wrapped coordinate can be found in the vertex
        // buffer the draw is about to consume, and watched there. The pool is CPU write-combine
        // and the UI is rebuilt every frame, so the producer writes this same place again
        // shortly - which is the whole point.
        // Only while aiming. Every UI quad in the game matches the shape test below, so
        // unfiltered this logs thousands of lines through boot and loading - enough to hit the
        // 15MB log cap before the crosshair is ever on screen, after which nothing is recorded
        // at all and the run looks like the diagnostics simply failed to fire.
        if (RenderPipeline::bWatchStagingWrites && !g_StagingWatchArmed.load()
            && g_AimHeld.load()
            && vertexCountPerInstance == 6 && g_LastVertexBuffer.StrideInBytes == 12
            && g_LastVertexBuffer.SizeInBytes > 0)
        {
            const UINT64 offset =
                static_cast<UINT64>(startVertexLocation) * g_LastVertexBuffer.StrideInBytes;

            const D3D12_GPU_VIRTUAL_ADDRESS vertexAddress =
                g_LastVertexBuffer.BufferLocation + offset;

            // The pool itself is GPU-only, so trace back through the copy that filled it.
            const uint8_t* vertices = ResolveVertexData(vertexAddress, 6 * 12);
            if (!vertices)
            {
                vertices = TraceVertexToStaging(vertexAddress, 6 * 12);
            }

            if (vertices)
            {
                std::string dump;
                for (int v = 0; v < 6; v++)
                {
                    int16_t x {};
                    int16_t y {};
                    std::memcpy(&x, vertices + (v * 12) + 0, sizeof(x));
                    std::memcpy(&y, vertices + (v * 12) + 2, sizeof(y));
                    dump += std::format(" ({:.1f},{:.1f})", x / 16.0, y / 16.0);
                }
                spdlog::info("MGS4: Traced quad to staging at 0x{:X}:{}",
                    reinterpret_cast<uintptr_t>(vertices), dump);

                TryArmStagingWatch(vertices, 6 * 12);
            }
        }

        // The UI quads are 6 vertices with a 12-byte stride. Their call stack points at the
        // engine code that positions them, which is where the 16-bit screen coordinate is
        // produced - the shader only applies a transform the CPU has already baked.
        if (RenderPipeline::bLogViewports
            && vertexCountPerInstance == 6 && g_LastVertexBuffer.StrideInBytes == 12)
        {
            // The anchor, read straight from where the input layout says it lives.
            //
            // Layout #2 of the reticle pipeline (the one built during gameplay) puts the
            // per-quad inputs on slot 1 as six float4s: v3 at 0, v4 at 16, v5 at 32, v6 at 48,
            // v7 at 64 and v8 at 80. v8 is the quad centre in NDC, which is the number that
            // decides where the reticle lands - so this is the value the whole hunt is for.
            //
            // Separate budget: the general UI sampling is drained by hundreds of unrelated
            // quads long before these appear.
            // Identified by the 96-byte slot-1 stream rather than by shader. The draws that
            // bind that stream are by definition the ones using layout #2, and the shader
            // attribution has already proved unreliable enough not to gate on it.
            // Read v6/v7/v8 from where this pipeline's input layout says they are.
            //
            // Two things had to be right here. The stride test is on SizeInBytes, not
            // StrideInBytes: slot 1 is bound with a stride of 0, 4 or 16 and never 96, because
            // a zero stride is how per-object constants are pushed through the input assembler,
            // and every probe requiring a 96-byte stride excluded exactly that case. And the
            // element offsets come from the layout rather than a constant 48/64/80 - pipelines
            // that place them elsewhere previously came back shifted by whole float4s, with v6
            // reading as (1,1,1) as the giveaway.
            const AnchorLocation anchor = CurrentPipelineAnchor();
            if (anchor.valid
                && anchor.slot < g_LastVertexStreamCount
                && g_LastVertexStreams[anchor.slot].BufferLocation != 0
                && g_AnchorBudget.fetch_sub(1) > 0)
            {
                const auto& view = g_LastVertexStreams[anchor.slot];
                const UINT64 needed = anchor.offsetV8 + 16;

                if (view.SizeInBytes >= needed)
                {
                    const UINT64 base = view.BufferLocation
                        + static_cast<UINT64>(startVertexLocation) * view.StrideInBytes;

                    // Which path resolved this matters for the watch, not just the read. A
                    // direct hit is a buffer we have mapped ourselves; a staging hit is the
                    // CPU-written source of a GPU copy. Watching the wrong one watches memory
                    // the CPU never touches, which would look exactly like the watch failing.
                    const char* source = "direct";
                    const uint8_t* data = ResolveVertexData(base, needed);
                    if (!data)
                    {
                        source = "staging";
                        data = TraceVertexToStaging(base, needed);
                    }

                    if (data)
                    {
                        float v6[3] {};
                        float v7[3] {};
                        float v8[3] {};
                        std::memcpy(v6, data + anchor.offsetV6, sizeof(v6));
                        std::memcpy(v7, data + anchor.offsetV7, sizeof(v7));
                        std::memcpy(v8, data + anchor.offsetV8, sizeof(v8));

                        // Validate the read before believing it.
                        //
                        // A correct anchor has an axis-aligned basis - v6 = (2/W, 0) and
                        // v7 = (0, -2/H) - and v8.z is exactly 1.0, which is consistent across
                        // every clean sample. A read that landed on the wrong staging copy fails
                        // all of these at once: v6 comes back (0,0), v7 (0,1), and v8.z equal to
                        // v8.x. Without this check, |v8| > 1.02 flagged 814 of 1614 anchors as
                        // wrapped when the true count is about five per frame, and the watch
                        // armed on a garbage address.
                        const bool plausible =
                            std::fabs(v6[1]) < 1e-6f && std::fabs(v7[0]) < 1e-6f
                            && v6[0] > 0.0f && v7[1] < 0.0f
                            && std::fabs(v8[2] - 1.0f) < 1e-4f;

                        const bool wrapped = plausible
                            && (std::fabs(v8[0]) > 1.02f || std::fabs(v8[1]) > 1.02f);

                        if (wrapped || (plausible && g_AnchorLogBudget.fetch_sub(1) > 0))
                        spdlog::info("ANCHOR{} {} vs={} slot={} off={}/{}/{} stride={} "
                            "v6=({:.5f},{:.5f}) v7=({:.5f},{:.5f}) v8=({:.5f},{:.5f},{:.3f}) "
                            "@0x{:X} via {}{}",
                            wrapped ? " WRAPPED" : "",
                            g_AimHeld.load() ? "AIM" : "IDLE",
                            CurrentVertexShaderChecksum().substr(0, 8),
                            anchor.slot, anchor.offsetV6, anchor.offsetV7, anchor.offsetV8,
                            view.StrideInBytes,
                            v6[0], v6[1], v7[0], v7[1], v8[0], v8[1], v8[2],
                            reinterpret_cast<uintptr_t>(data + anchor.offsetV8), source,
                            DescribeGameCallers());

                        // An implausible read used to vanish without trace, and so did a
                        // sampler with nothing in its pipeline map - both look identical to
                        // "no anchors this run" from the outside. A few examples of what was
                        // actually read make a wrong offset or a wrong stream diagnosable
                        // from the log instead of a mystery.
                        if (!plausible)
                        {
                            static std::atomic<int> implausibleShown { 0 };
                            if (implausibleShown.fetch_add(1) < 8)
                            {
                                spdlog::info("ANCHOR IMPLAUSIBLE {} vs={} slot={} off={}/{}/{} stride={} "
                                    "v6=({:.5f},{:.5f}) v7=({:.5f},{:.5f}) v8=({:.5f},{:.5f},{:.5f}) via {}",
                                    g_AimHeld.load() ? "AIM" : "IDLE",
                                    CurrentVertexShaderChecksum().substr(0, 8),
                                    anchor.slot, anchor.offsetV6, anchor.offsetV7, anchor.offsetV8,
                                    view.StrideInBytes,
                                    v6[0], v6[1], v7[0], v7[1], v8[0], v8[1], v8[2], source);
                            }
                        }

                        // A wrapped anchor is the whole point: watch the exact address it was
                        // read from, so the next frame's write to it names the producer. Only
                        // the wrapped one is worth arming - the correct anchors are written by
                        // the same code doing the right thing, and there is one watch to spend.
                        // Arm on the reticle itself. The first wrapped element encountered is
                        // usually some other overlay; the crosshair goes out through +7A5DA3,
                        // which is the path worth spending the single watch on.
                        // Go to the source array instead of the vertex buffer. The vertex
                        // buffer is a per-frame ring, so a watch there catches at best a stale
                        // slot; the array it is copied from is a stable allocation.
                        if (wrapped)
                        {
                            static std::atomic<bool> scanned { false };
                            bool expected = false;
                            if (scanned.compare_exchange_strong(expected, true))
                            {
                                // Off the render thread: the sweep walks the address space and
                                // would stall the frame badly if done inline.
                                std::thread(ScanProcessForWrappedPair).detach();
                            }
                        }

                        if (false && wrapped && UiDrawCallSite() == "7A5DA3")
                        {
                            ArmAnchorWatch(
                                reinterpret_cast<uintptr_t>(data + anchor.offsetV8),
                                "a wrapped reticle anchor");
                        }
                    }
                }
            }

            // Sample while aiming, not from process start. Taking the first draws
            // unconditionally captured the main menu's UI instead of the crosshair - a
            // different pool entirely, which is why they never resolved. Tying it to the aim
            // button means the sample is of the thing being investigated.
            // Aiming and idle are sampled separately so the two can be diffed. The && chain
            // short-circuits on g_AimHeld, so exactly one of the two budgets is decremented
            // per draw - they cannot drain each other.
            const bool aiming = g_AimHeld.load();
            const bool takeAiming = RenderPipeline::bWatchStagingWrites
                && aiming && g_AimSampleBudget.fetch_sub(1) > 0;
            const bool takeIdle = RenderPipeline::bWatchStagingWrites
                && !aiming && g_IdleSampleBudget.fetch_sub(1) > 0;

            if (takeAiming || takeIdle || g_StateLogBudget.fetch_sub(1) > 0)
            {
                const char* phase = takeAiming ? "AIM" : (takeIdle ? "IDLE" : "STATE");
                // The draw starts at startVertexLocation, not at the front of the pool. Reading
                // from BufferLocation printed vertex 0 of a shared 3MB buffer - some other
                // quad entirely - which made these numbers untrustworthy for any draw with a
                // non-zero start.
                const UINT64 drawOffset =
                    static_cast<UINT64>(startVertexLocation) * g_LastVertexBuffer.StrideInBytes;

                // The pool is a GPU-only CUSTOM heap, so the direct read fails and the copy it
                // was filled from has to be walked instead.
                const uint8_t* vertices =
                    ResolveVertexData(g_LastVertexBuffer.BufferLocation + drawOffset, 6 * 12);
                if (!vertices)
                {
                    vertices = TraceVertexToStaging(
                        g_LastVertexBuffer.BufferLocation + drawOffset, 6 * 12);
                }

                // Summarise every UI draw in one line, and only dump a full one when it looks
                // like the reticle. A frame of UI is hundreds of draws; dumping each in full
                // overruns the 15MB log cap long before the interesting one arrives, which is
                // how the earlier captures ended up recording everything except the target.
                //
                // Units are 2 per buffer pixel (cb[0] == 1/(2*bufferWidth)), so a full-screen
                // quad spans 0..2*width and the reticle is a small box about the centre.
                int32_t minX = INT32_MAX;
                int32_t minY = INT32_MAX;
                int32_t maxX = INT32_MIN;
                int32_t maxY = INT32_MIN;

                // Vertex layout, 12 bytes: [int16 u][int16 v][RGBA8 colour][int16 x][int16 y].
                //
                // The screen position is at offset 8, NOT offset 0. Offset 0 is the texture
                // coordinate. Everything read from offset 0 up to now was measuring the sprite's
                // place in its atlas, which is why no draw ever appeared at screen centre and
                // why over a thousand draws looked "reticle-sized" - atlas tiles are all small.
                //
                // Full-screen blits happen to carry the same values in both pairs, which is what
                // made the wrong field look plausible.
                if (vertices)
                {
                    for (int v = 0; v < 6; v++)
                    {
                        int16_t x {};
                        int16_t y {};
                        std::memcpy(&x, vertices + (v * 12) + 8, sizeof(x));
                        std::memcpy(&y, vertices + (v * 12) + 10, sizeof(y));
                        minX = std::min<int32_t>(minX, x);
                        maxX = std::max<int32_t>(maxX, x);
                        minY = std::min<int32_t>(minY, y);
                        maxY = std::max<int32_t>(maxY, y);
                    }
                }

                const int32_t extentX = vertices ? (maxX - minX) : -1;
                const int32_t extentY = vertices ? (maxY - minY) : -1;

                // Vertex colour, from offset 4. The reticle is a distinctly orange element and
                // almost nothing else in the HUD shares that colour, so this identifies it far
                // more sharply than size or position - both of which have already produced
                // plausible-looking wrong answers.
                uint32_t colour = 0;
                if (vertices)
                {
                    std::memcpy(&colour, vertices + 4, sizeof(colour));
                }

                // A reticle-sized quad: small, and not the degenerate zero-extent quads that
                // make up most of the HUD's filler geometry.
                const bool reticleSized = vertices
                    && extentX > 0 && extentY > 0
                    && extentX < 600 && extentY < 600;

                const bool isReticleShader =
                    CurrentVertexShaderChecksum() == kReticleVertexShaderChecksum;

                // One stable line per draw, carrying the call site. Stable is the point: these
                // logs get diffed between a 100% and a 200% run, and the reticle is whichever
                // draw does NOT simply double when the buffer doubles. Size alone cannot pick
                // it out - over a thousand draws are reticle-sized, and their coordinates turn
                // out to be texture UVs rather than screen positions.
                if (vertices)
                {
                    spdlog::info("UIQUAD {} vs={} layout={} site={} x[{},{}] y[{},{}] "
                        "ext={}x{} mid=({},{}) col=0x{:08X}",
                        phase, 
                        CurrentVertexShaderChecksum().substr(0, 8),
                        CurrentPipelineLayoutId(),
                        UiDrawCallSite(),
                        minX, maxX, minY, maxY, extentX, extentY,
                        (minX + maxX) / 2, (minY + maxY) / 2, colour);
                }
                else
                {
                    spdlog::info("UIQUAD {} vs={} site={} unresolvable", phase,
                        CurrentVertexShaderChecksum().substr(0, 8), UiDrawCallSite());
                }

                // The full per-vertex and constant-buffer dump is expensive and only worth it
                // for a plausible reticle.
                if (!isReticleShader || !reticleSized)
                {
                    DrawInstanced_hook.stdcall<void>(self, vertexCountPerInstance, instanceCount,
                        startVertexLocation, startInstanceLocation);
                    return;
                }

                // Every stream bound for this draw. The int16 position and the float anchor
                // live in different ones, so seeing them together shows whether the anchor was
                // derived from an already-wrapped coordinate or wrapped independently.
                for (UINT stream = 0; stream < g_LastVertexStreamCount; stream++)
                {
                    const auto& view = g_LastVertexStreams[stream];
                    if (view.BufferLocation == 0 || view.StrideInBytes == 0)
                    {
                        continue;
                    }

                    const UINT64 offset =
                        static_cast<UINT64>(startVertexLocation) * view.StrideInBytes;
                    const UINT64 span =
                        static_cast<UINT64>(vertexCountPerInstance) * view.StrideInBytes;
                    // The UI pool is a CUSTOM heap with CPUPageProperty NOT_AVAILABLE, so it is
                    // GPU-only and ResolveVertexData can never read it. The staging buffer it
                    // was copied from is CPU-visible, though, so fall back to walking the
                    // recorded CopyBufferRegion calls - that is what actually resolves here.
                    const uint8_t* data =
                        ResolveVertexData(view.BufferLocation + offset, span);
                    if (!data)
                    {
                        data = TraceVertexToStaging(view.BufferLocation + offset, span);
                    }

                    spdlog::info("    stream {}: gpu 0x{:X}, stride {:2}, size {:6}{}",
                        stream, view.BufferLocation, view.StrideInBytes, view.SizeInBytes,
                        data ? "" : "  <not in a mapped buffer>");

                    if (!data)
                    {
                        continue;
                    }

                    // Every vertex of the quad, not just the first. This is the whole point of
                    // the dump: a per-quad translate is identical across all six vertices,
                    // while a per-vertex position varies. Reading only vertex 0 cannot tell
                    // those apart, and telling them apart is what identifies the anchor.
                    for (UINT v = 0; v < vertexCountPerInstance; v++)
                    {
                        const uint8_t* vertex = data + (static_cast<size_t>(v) * view.StrideInBytes);

                        std::string raw;
                        for (UINT b = 0; b < view.StrideInBytes && b < 32; b++)
                        {
                            raw += std::format("{:02X} ", vertex[b]);
                        }

                        // Both readings, since which one is meaningful depends on the stream.
                        std::string decoded;
                        if (view.StrideInBytes >= 4)
                        {
                            int16_t a {};
                            int16_t c {};
                            std::memcpy(&a, vertex, sizeof(a));
                            std::memcpy(&c, vertex + 2, sizeof(c));
                            decoded += std::format("| i16 {},{} ({:.1f},{:.1f}px)",
                                a, c, a / 16.0, c / 16.0);
                        }
                        if (view.StrideInBytes >= 8)
                        {
                            float fx {};
                            float fy {};
                            std::memcpy(&fx, vertex, sizeof(fx));
                            std::memcpy(&fy, vertex + 4, sizeof(fy));
                            decoded += std::format(" | f32 {:.4f},{:.4f}", fx, fy);
                        }

                        spdlog::info("      v{}: {}{}", v, raw, decoded);
                    }
                }

                // With only one vertex stream bound, and that stream carrying a small quad in
                // local units, the per-quad translate cannot be coming from vertex data at
                // all. The root constant buffer is the remaining place it can live, so read it
                // here rather than only on the unrelated state-log path.
                // Root 32-bit constants first: a small per-draw transform is most naturally
                // passed this way, and nothing was recording them until now.
                for (UINT slot = 0; slot < kMaxRootParameters; slot++)
                {
                    const RootConstantRecord& record = g_RootConstants[slot];
                    if (record.count == 0)
                    {
                        continue;
                    }

                    std::string line;
                    for (UINT i = 0; i < record.count && i < 24; i++)
                    {
                        float asFloat {};
                        std::memcpy(&asFloat, &record.values[i], sizeof(asFloat));
                        line += std::format("  [{}] 0x{:08X} {:g}", i, record.values[i], asFloat);
                    }
                    spdlog::info("      root const slot {} ({} values):{}",
                        slot, record.count, line);
                }

                for (UINT slot = 0; slot < kMaxRootParameters; slot++)
                {
                    if (g_RootCBV[slot] == 0)
                    {
                        continue;
                    }

                    const uint8_t* buffer = ResolveVertexData(g_RootCBV[slot], 64);
                    if (!buffer)
                    {
                        buffer = TraceVertexToStaging(g_RootCBV[slot], 64);
                    }

                    if (!buffer)
                    {
                        spdlog::info("      root cbv slot {} @0x{:X}: <unresolvable>",
                            slot, g_RootCBV[slot]);
                        continue;
                    }

                    std::string line;
                    for (int i = 0; i < 16; i++)
                    {
                        float value {};
                        std::memcpy(&value, buffer + (static_cast<size_t>(i) * 4), sizeof(value));
                        line += std::format(" {:g}", value);
                    }
                    spdlog::info("      root cbv slot {} @0x{:X}:{}", slot, g_RootCBV[slot], line);
                }

                if (g_LastRootCBV != 0)
                {
                    const uint8_t* cb = ResolveVertexData(g_LastRootCBV, 128);
                    if (!cb)
                    {
                        cb = TraceVertexToStaging(g_LastRootCBV, 128);
                    }

                    if (cb)
                    {
                        // Print in rows of four, so a 4x4 transform reads as a matrix and the
                        // translate column is obvious rather than buried in a flat list.
                        // Full precision, plus the reciprocal. These values are expected to be
                        // scale factors of the form 1/N, and at four decimal places 1/7680 and
                        // 1/4320 both collapse to "0.0001"-ish - which loses exactly the number
                        // that identifies the coordinate space. The reciprocal is what makes
                        // the space readable at a glance.
                        for (int row = 0; row < 8; row++)
                        {
                            float f[4] {};
                            std::memcpy(f, cb + (static_cast<size_t>(row) * 16), sizeof(f));

                            std::string line;
                            for (int i = 0; i < 4; i++)
                            {
                                line += std::format("  {:.9g}", f[i]);
                                if (f[i] != 0.0f && std::fabs(f[i]) < 0.5f)
                                {
                                    line += std::format(" (1/{:.4g})", 1.0f / f[i]);
                                }
                            }

                            spdlog::info("      cb[{}..{}]:{}", row * 4, row * 4 + 3, line);
                        }
                    }
                    else
                    {
                        spdlog::info("      cb at 0x{:X}: <not resolvable>", g_LastRootCBV);
                    }
                }
            }
        }

        if (TakeStateLogBudget())
        {
            std::string vertices;

            // Small vertex counts with a float3 stride are the UI quads we care about;
            // dump their actual positions rather than just the counts.
            if (vertexCountPerInstance <= 8 && g_LastVertexBuffer.StrideInBytes == 12)
            {
                const UINT64 offset = static_cast<UINT64>(startVertexLocation) * g_LastVertexBuffer.StrideInBytes;
                const UINT64 span = static_cast<UINT64>(vertexCountPerInstance) * g_LastVertexBuffer.StrideInBytes;

                if (const uint8_t* data = ResolveVertexData(g_LastVertexBuffer.BufferLocation + offset, span))
                {
                    for (UINT v = 0; v < vertexCountPerInstance; v++)
                    {
                        float xyz[3] {};
                        std::memcpy(xyz, data + (static_cast<size_t>(v) * 12), sizeof(xyz));
                        vertices += std::format(" ({:.1f},{:.1f},{:.2f})", xyz[0], xyz[1], xyz[2]);
                    }
                }
                else
                {
                    vertices = " <vertex data not resolvable>";
                }

                // The constant buffer is the other place a screen-size or transform could
                // come from, and unlike the vertex buffer it is usually CPU-visible.
                if (g_LastRootCBV != 0)
                {
                    if (const uint8_t* cb = ResolveVertexData(g_LastRootCBV, 64))
                    {
                        vertices += "  cb:";
                        for (int f = 0; f < 16; f++)
                        {
                            float value = 0.0f;
                            std::memcpy(&value, cb + (static_cast<size_t>(f) * 4), sizeof(value));
                            vertices += std::format(" {:.3f}", value);
                        }
                    }
                    else
                    {
                        vertices += "  <cb not resolvable>";
                    }
                }
            }

            spdlog::info("MGS4: [{}] Draw verts={} instances={} startVert={} vbStride={} vbSize={}{}",
                g_StateLogSeq.fetch_add(1),
                vertexCountPerInstance, instanceCount, startVertexLocation,
                g_LastVertexBuffer.StrideInBytes, g_LastVertexBuffer.SizeInBytes, vertices);
        }

        DrawInstanced_hook.stdcall<void>(self, vertexCountPerInstance, instanceCount,
            startVertexLocation, startInstanceLocation);
    }

    HRESULT STDMETHODCALLTYPE Hooked_CreateCommandList(ID3D12Device* self, UINT nodeMask,
        D3D12_COMMAND_LIST_TYPE type, ID3D12CommandAllocator* allocator,
        ID3D12PipelineState* initialState, REFIID riid, void** commandList)
    {
        const HRESULT result = CreateCommandList_hook.stdcall<HRESULT>(self, nodeMask, type,
            allocator, initialState, riid, commandList);

        // Command lists all share one vtable, so hooking the first is enough.
        static bool bHooked = false;
        if (SUCCEEDED(result) && commandList && *commandList && !bHooked)
        {
            bHooked = true;
            void** vtable = *reinterpret_cast<void***>(*commandList);

            RSSetViewports_hook = safetyhook::create_inline(vtable[kRSSetViewportsSlot],
                reinterpret_cast<void*>(Hooked_RSSetViewports));
            RSSetScissorRects_hook = safetyhook::create_inline(vtable[kRSSetScissorRectsSlot],
                reinterpret_cast<void*>(Hooked_RSSetScissorRects));

            DrawInstanced_hook = safetyhook::create_inline(vtable[kDrawInstancedSlot],
                reinterpret_cast<void*>(Hooked_DrawInstanced));
            DrawIndexedInstanced_hook = safetyhook::create_inline(vtable[kDrawIndexedInstancedSlot],
                reinterpret_cast<void*>(Hooked_DrawIndexedInstanced));
            spdlog::info("MGS4: D3D12 DrawIndexedInstanced hook: {}.",
                DrawIndexedInstanced_hook ? "installed" : "FAILED");
            IASetVertexBuffers_hook = safetyhook::create_inline(vtable[kIASetVertexBuffersSlot],
                reinterpret_cast<void*>(Hooked_IASetVertexBuffers));
            SetGraphicsRootCBV_hook = safetyhook::create_inline(vtable[kSetGraphicsRootCBVSlot],
                reinterpret_cast<void*>(Hooked_SetGraphicsRootCBV));
            SetGraphicsRoot32BitConstants_hook = safetyhook::create_inline(
                vtable[kSetGraphicsRoot32BitConstantsSlot],
                reinterpret_cast<void*>(Hooked_SetGraphicsRoot32BitConstants));
            SetGraphicsRoot32BitConstant_hook = safetyhook::create_inline(
                vtable[kSetGraphicsRoot32BitConstantSlot],
                reinterpret_cast<void*>(Hooked_SetGraphicsRoot32BitConstant));
            CopyBufferRegion_hook = safetyhook::create_inline(vtable[kCopyBufferRegionSlot],
                reinterpret_cast<void*>(Hooked_CopyBufferRegion));
            SetPipelineState_hook = safetyhook::create_inline(vtable[kSetPipelineStateSlot],
                reinterpret_cast<void*>(Hooked_SetPipelineState));

            spdlog::info("MGS4: Viewport/scissor logging: {} / {}. Draw logging: {} / {}.",
                RSSetViewports_hook ? "installed" : "FAILED",
                RSSetScissorRects_hook ? "installed" : "FAILED",
                DrawInstanced_hook ? "installed" : "FAILED",
                IASetVertexBuffers_hook ? "installed" : "FAILED");
        }

        return result;
    }

    HRESULT WINAPI Hooked_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL minimumFeatureLevel,
        REFIID riid, void** device)
    {
        const HRESULT result = D3D12CreateDevice_hook.stdcall<HRESULT>(adapter, minimumFeatureLevel, riid, device);

        // D3D12CreateDevice is also legitimately called with a null device pointer purely
        // as a capability check, so only act on a call that actually produced a device.
        static bool bDeviceHooked = false;
        if (SUCCEEDED(result) && device && *device && !bDeviceHooked)
        {
            bDeviceHooked = true;

            void** vtable = *reinterpret_cast<void***>(*device);
            spdlog::info("MGS4: D3D12 device created, hooking resource creation on its vtable.");

            CreateCommittedResource_hook = safetyhook::create_inline(vtable[kCreateCommittedResourceSlot],
                reinterpret_cast<void*>(Hooked_CreateCommittedResource));
            spdlog::info("MGS4: D3D12 CreateCommittedResource hook: {}.",
                CreateCommittedResource_hook ? "installed" : "FAILED");

            if (RenderPipeline::iAnisotropicFiltering > 0)
            {
                CreateSampler_hook = safetyhook::create_inline(vtable[kCreateSamplerSlot],
                    reinterpret_cast<void*>(Hooked_CreateSampler));
                spdlog::info("MGS4: D3D12 CreateSampler hook: {}.",
                    CreateSampler_hook ? "installed" : "FAILED");
            }

            // Only hook the newer entry points if the device actually implements them, or the
            // vtable slots belong to something else entirely.
            {
                ID3D12Device* probe = nullptr;
                auto* const dev = static_cast<ID3D12Device*>(*device);
                if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device4),
                    reinterpret_cast<void**>(&probe))) && probe)
                {
                    probe->Release();
                    CreateCommittedResource1_hook = safetyhook::create_inline(
                        vtable[kCreateCommittedResource1Slot],
                        reinterpret_cast<void*>(Hooked_CreateCommittedResource1));
                    spdlog::info("MGS4: D3D12 CreateCommittedResource1 hook: {}.",
                        CreateCommittedResource1_hook ? "installed" : "FAILED");
                }

                probe = nullptr;
                if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D12Device8),
                    reinterpret_cast<void**>(&probe))) && probe)
                {
                    probe->Release();
                    CreateCommittedResource2_hook = safetyhook::create_inline(
                        vtable[kCreateCommittedResource2Slot],
                        reinterpret_cast<void*>(Hooked_CreateCommittedResource2));
                    spdlog::info("MGS4: D3D12 CreateCommittedResource2 hook: {}.",
                        CreateCommittedResource2_hook ? "installed" : "FAILED");
                }
            }

            CreatePlacedResource_hook = safetyhook::create_inline(vtable[kCreatePlacedResourceSlot],
                reinterpret_cast<void*>(Hooked_CreatePlacedResource));
            spdlog::info("MGS4: D3D12 CreatePlacedResource hook: {}.",
                CreatePlacedResource_hook ? "installed" : "FAILED");

            if (RenderPipeline::bLogViewports)
            {
                CreateGraphicsPipelineState_hook = safetyhook::create_inline(vtable[kCreateGraphicsPipelineStateSlot],
                    reinterpret_cast<void*>(Hooked_CreateGraphicsPipelineState));
            }

            if (RenderPipeline::bLogViewports)
            {
                CreateCommandList_hook = safetyhook::create_inline(vtable[kCreateCommandListSlot],
                    reinterpret_cast<void*>(Hooked_CreateCommandList));
                spdlog::info("MGS4: D3D12 CreateCommandList hook: {}.",
                    CreateCommandList_hook ? "installed" : "FAILED");
                StartStateLogHotkey();
            }
        }

        return result;
    }

    // D3D11 device creation. The only thing wanted from a D3D11 device is the sampler hook;
    // the allocation logging above is a D3D12 diagnostic.
    SafetyHookInline D3D11CreateDevice_hook {};
    SafetyHookInline D3D11CreateDeviceAndSwapChain_hook {};
    constexpr size_t kCreateSamplerStateSlot = 23;

    void HookD3D11Device(ID3D11Device* device)
    {
        static bool hooked = false;
        if (hooked || !device)
        {
            return;
        }
        hooked = true;
        spdlog::info("MGS4: D3D11 device created.");

        if (RenderPipeline::iAnisotropicFiltering > 0)
        {
            void** vtable = *reinterpret_cast<void***>(device);
            CreateSamplerState_hook = safetyhook::create_inline(vtable[kCreateSamplerStateSlot],
                reinterpret_cast<void*>(Hooked_CreateSamplerState));
            spdlog::info("MGS4: D3D11 CreateSamplerState hook: {}.", CreateSamplerState_hook ? "installed" : "FAILED");
        }
    }

    HRESULT WINAPI Hooked_D3D11CreateDevice(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
        UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount, UINT sdkVersion,
        ID3D11Device** device, D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context)
    {
        const HRESULT result = D3D11CreateDevice_hook.stdcall<HRESULT>(adapter, driverType, software, flags,
            featureLevels, featureLevelCount, sdkVersion, device, featureLevel, context);
        if (SUCCEEDED(result) && device && *device)
        {
            HookD3D11Device(*device);
        }
        return result;
    }

    HRESULT WINAPI Hooked_D3D11CreateDeviceAndSwapChain(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
        HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* featureLevels, UINT featureLevelCount, UINT sdkVersion,
        const DXGI_SWAP_CHAIN_DESC* swapChainDesc, IDXGISwapChain** swapChain, ID3D11Device** device,
        D3D_FEATURE_LEVEL* featureLevel, ID3D11DeviceContext** context)
    {
        const HRESULT result = D3D11CreateDeviceAndSwapChain_hook.stdcall<HRESULT>(adapter, driverType, software,
            flags, featureLevels, featureLevelCount, sdkVersion, swapChainDesc, swapChain, device, featureLevel, context);
        if (SUCCEEDED(result) && device && *device)
        {
            HookD3D11Device(*device);
        }
        return result;
    }

    // The game's fullscreen resolution is the largest display mode the monitor lists: at
    // start-up it walks EnumDisplaySettingsW over every mode and keeps the biggest, uses that
    // as its window size and, at the fullscreen step, switches the display to it (a real mode
    // change - measured 2026-09-05 on a 1600x1200 output that was switched to 1920x1080, its
    // largest listed mode, with the picture then squeezed into a 16:9 mode on a 4:3 screen).
    // With the Window Aspect Ratio override on, the walk is answered with the current mode
    // only, so the game's "largest" is the desktop's mode, the window is the desktop, and the
    // mode switch is a no-op. The fit in the ultrawide module then places the chosen picture
    // in that window. Queries for the current or registry mode (negative indices) pass through.
    SafetyHookInline EnumDisplaySettingsW_hook {};
    SafetyHookInline EnumDisplaySettingsExW_hook {};
    std::atomic<int> g_ModeWalkReported { 0 };

    BOOL WINAPI Hooked_EnumDisplaySettingsW(LPCWSTR device, DWORD mode, DEVMODEW* dm)
    {
        if (static_cast<int>(mode) >= 0 && RenderPipeline::bOverrideWindowSize)
        {
            if (mode != 0)
            {
                return FALSE;
            }
            const BOOL r = EnumDisplaySettingsW_hook.stdcall<BOOL>(device, ENUM_CURRENT_SETTINGS, dm);
            if (r && dm && g_ModeWalkReported.fetch_add(1) == 0)
            {
                spdlog::info("MGS4: Window Size: the display mode list is answered with the current mode only ({}x{}), so the game keeps the desktop's mode.",
                    dm->dmPelsWidth, dm->dmPelsHeight);
            }
            return r;
        }
        return EnumDisplaySettingsW_hook.stdcall<BOOL>(device, mode, dm);
    }

    BOOL WINAPI Hooked_EnumDisplaySettingsExW(LPCWSTR device, DWORD mode, DEVMODEW* dm, DWORD flags)
    {
        if (static_cast<int>(mode) >= 0 && RenderPipeline::bOverrideWindowSize)
        {
            if (mode != 0)
            {
                return FALSE;
            }
            return EnumDisplaySettingsExW_hook.stdcall<BOOL>(device, ENUM_CURRENT_SETTINGS, dm, flags);
        }
        return EnumDisplaySettingsExW_hook.stdcall<BOOL>(device, mode, dm, flags);
    }

    // The renderer's own resolution (AR-019, 2026-09-07). The engine keeps a second copy of
    // its resolution inside the renderer context: the init record's width and height, which
    // the per-frame submit copies into each frame ("movups [frame+0x24D35C8], xmm0" from
    // context+0x4C12308 at +76E010) and the backends then use to size their buffers, to
    // clamp view rects and to ask for the swap chain resize. The window-size override never
    // reached that copy, so with the in-game resolution set below the override - windowed
    // mode, 2560x1440 chosen in-game under a 3200x1800 override - the window was ours and the
    // renderer's buffers and clamp were the game's: draws dropped on D3D11 (black), clamped
    // on D3D12 (misaligned). The submit is hooked and the record is written from the window
    // size globals before it runs, so every frame's resolution is the window's. In fullscreen
    // the chain still follows the desktop (SizeToWindow in the private module) and the fit
    // places the picture, as before.
    constexpr uintptr_t kRvaRendererSubmit = 0x76DFF0;
    constexpr uintptr_t kContextResolutionWidth = 0x4C1230C;
    constexpr uintptr_t kContextResolutionHeight = 0x4C12310;
    SafetyHookInline RendererSubmit_hook {};
    std::atomic<int> g_RendererResolutionReported { 0 };
    const char* SurfaceSize(int& width, int& height);

    uint32_t __fastcall Hooked_RendererSubmit(uint8_t* context)
    {
        // The value written is the game window's client area, which is what the swap chain
        // is sized to in both modes: the override in windowed mode, the desktop in
        // fullscreen. Writing the override itself was wrong in fullscreen (2026-09-07): the
        // renderer's buffers came out smaller than the chain and D3D11 drew nothing. The
        // renderer's resolution must never be smaller than the chain; equal is right.
        if (context && RenderPipeline::bOverrideWindowSize)
        {
            int w = 0, h = 0;
            if (SurfaceSize(w, h) && w > 0 && h > 0)
            {
                int32_t current[2] {};
                std::memcpy(&current[0], context + kContextResolutionWidth, sizeof(int32_t));
                std::memcpy(&current[1], context + kContextResolutionHeight, sizeof(int32_t));
                if (current[0] != w || current[1] != h)
                {
                    if (g_RendererResolutionReported.fetch_add(1) < 4)
                    {
                        spdlog::info("MGS4: Window Size: the renderer's own resolution {}x{} -> {}x{} (the window's client area, which is the chain).", current[0], current[1], w, h);
                    }
                    const int32_t nw = w, nh = h;
                    std::memcpy(context + kContextResolutionWidth, &nw, sizeof(int32_t));
                    std::memcpy(context + kContextResolutionHeight, &nh, sizeof(int32_t));
                }
            }
        }
        return RendererSubmit_hook.fastcall<uint32_t>(context);
    }

    void InstallRendererResolution()
    {
        auto* const at = reinterpret_cast<uint8_t*>(mgs4e::game::Module()) + kRvaRendererSubmit;
        // mov [rsp+8],rbx; mov [rsp+10h],rsi; push rdi; sub rsp,20h; mov rdi,rcx - the prologue seen in-process.
        constexpr uint8_t kExpected[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9 };
        if (!mgs4e::mem::Readable(at, sizeof(kExpected)) || std::memcmp(at, kExpected, sizeof(kExpected)) != 0)
        {
            spdlog::warn("MGS4: Window Size: the renderer submit at +{:X} does not look as expected - wrong executable build; the renderer's resolution is left to the game.", kRvaRendererSubmit);
            return;
        }
        RendererSubmit_hook = safetyhook::create_inline(at, reinterpret_cast<void*>(&Hooked_RendererSubmit));
        spdlog::info("MGS4: Window Size: renderer resolution hook {}.", RendererSubmit_hook ? "installed" : "FAILED");
    }

    void InstallDisplayModeClamp()
    {
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32)
        {
            return;
        }
        if (void* fn = reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsW")))
        {
            EnumDisplaySettingsW_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_EnumDisplaySettingsW));
        }
        if (void* fn = reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsExW")))
        {
            EnumDisplaySettingsExW_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_EnumDisplaySettingsExW));
        }
        spdlog::info("MGS4: Window Size: display mode clamp {} (EnumDisplaySettingsW {}, ExW {}).",
            EnumDisplaySettingsW_hook ? "installed" : "FAILED", EnumDisplaySettingsW_hook ? "ok" : "no", EnumDisplaySettingsExW_hook ? "ok" : "no");
    }

    // The surface the window override must fit into: the game's own top-level window's
    // client area once the window exists (windowed mode sizes it from the in-game setting;
    // fullscreen makes it the desktop), else the primary desktop's current mode. The name
    // says which one was used.
    BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM param)
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) { return TRUE; }
        RECT client {};
        if (!GetClientRect(hwnd, &client) || client.right < 64 || client.bottom < 64) { return TRUE; }
        *reinterpret_cast<HWND*>(param) = hwnd;
        return FALSE;
    }

    const char* SurfaceSize(int& width, int& height)
    {
        // Called once a frame from the renderer submit hook: the window is remembered and
        // only looked up again if it has gone.
        static HWND cached = nullptr;
        HWND game = (cached && IsWindow(cached)) ? cached : nullptr;
        if (!game)
        {
            EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&game));
            cached = game;
        }
        RECT client {};
        if (game && GetClientRect(game, &client) && client.right > 0 && client.bottom > 0)
        {
            width = client.right;
            height = client.bottom;
            return "window";
        }
        DEVMODEW mode {};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmPelsWidth > 0 && mode.dmPelsHeight > 0)
        {
            width = static_cast<int>(mode.dmPelsWidth);
            height = static_cast<int>(mode.dmPelsHeight);
            return "desktop";
        }
        return nullptr;
    }

    void InstallRenderTargetLogging()
    {
        // Loading d3d12.dll early is harmless - when the game later resolves it, it
        // gets this same already-loaded module and calls through our hook.
        const HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        if (!d3d12)
        {
            spdlog::error("MGS4: D3D12 diagnostics: failed to load d3d12.dll.");
            return;
        }

        void* createDevice = reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"));
        if (!createDevice)
        {
            spdlog::error("MGS4: D3D12 diagnostics: could not resolve D3D12CreateDevice.");
            return;
        }

        D3D12CreateDevice_hook = safetyhook::create_inline(createDevice,
            reinterpret_cast<void*>(Hooked_D3D12CreateDevice));
        spdlog::info("MGS4: D3D12CreateDevice hook: {}.", D3D12CreateDevice_hook ? "installed" : "FAILED");

        // And D3D11, which the game ships set to (mgs4.ecf: api = dx11) and offers in its menu.
        if (const HMODULE d3d11 = LoadLibraryW(L"d3d11.dll"))
        {
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice")))
            {
                D3D11CreateDevice_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_D3D11CreateDevice));
            }
            if (void* fn = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain")))
            {
                D3D11CreateDeviceAndSwapChain_hook = safetyhook::create_inline(fn, reinterpret_cast<void*>(Hooked_D3D11CreateDeviceAndSwapChain));
            }
            spdlog::info("MGS4: D3D11CreateDevice hook: {}.", D3D11CreateDevice_hook ? "installed" : "FAILED");
        }
    }

    // Collects the readable address ranges of mgs4.exe below .data, i.e. the code and
    // read-only data. Keeps the scans below from having to probe page by page.
    std::vector<std::pair<uint8_t*, uint8_t*>> GetCodeAndRodataRanges()
    {
        std::vector<std::pair<uint8_t*, uint8_t*>> ranges;

        MODULEINFO moduleInfo {};
        if (!GetModuleInformation(GetCurrentProcess(), mgs4e::game::Module(), &moduleInfo, sizeof(moduleInfo)))
        {
            return ranges;
        }

        auto* const imageStart = static_cast<uint8_t*>(moduleInfo.lpBaseOfDll);
        // .text and .rdata both sit below .data, so this covers the string literal and
        // every instruction that could reference it, without walking 500MB of globals.
        uint8_t* const limit = imageStart + (std::min)(static_cast<size_t>(moduleInfo.SizeOfImage),
            static_cast<size_t>(0x1B00000));

        for (uint8_t* cursor = imageStart; cursor < limit; )
        {
            MEMORY_BASIC_INFORMATION mbi {};
            if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize == 0)
            {
                break;
            }

            uint8_t* regionEnd = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd > limit)
            {
                regionEnd = limit;
            }

            if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0 && regionEnd > cursor)
            {
                ranges.emplace_back(cursor, regionEnd);
            }

            cursor = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        }

        return ranges;
    }

    // Resolves a render config global by meaning rather than by shape: the config key
    // string is referenced exactly once in the code, and the first "MOV [rip], EAX" after
    // that reference is the global the parsed value is stored into.
    //
    // This exists because the byte signature used to find the hook site is generic enough
    // to match ten places in .text - five of them inside the parse run itself, since the
    // sequence repeats every 11 bytes and the pattern overlaps itself. Structural checks
    // alone can't separate the right one from a similar config-parse run elsewhere, so the
    // strings give us an unambiguous answer to validate against.
    int* ResolveRenderConfigGlobal(const char* key)
    {
        const auto ranges = GetCodeAndRodataRanges();
        if (ranges.empty())
        {
            return nullptr;
        }

        const size_t keyLength = std::strlen(key);

        const uint8_t* keyAddress = nullptr;
        for (const auto& [start, end] : ranges)
        {
            for (uint8_t* p = start; p + keyLength + 1 < end; p++)
            {
                if (*p == static_cast<uint8_t>(key[0])
                    && std::memcmp(p, key, keyLength) == 0
                    && p[keyLength] == '\0')
                {
                    keyAddress = p;
                    break;
                }
            }
            if (keyAddress)
            {
                break;
            }
        }

        if (!keyAddress)
        {
            return nullptr;
        }

        // Any 4-byte field whose RIP-relative target is the string is a reference to it.
        for (const auto& [start, end] : ranges)
        {
            for (uint8_t* p = start; p + 4 < end; p++)
            {
                int32_t displacement = 0;
                std::memcpy(&displacement, p, sizeof(displacement));
                if (p + 4 + displacement != keyAddress)
                {
                    continue;
                }

                for (uint8_t* q = p; q + 6 < end && q < p + 96; q++)
                {
                    if (q[0] == 0x89 && q[1] == 0x05) // MOV [rip+disp32], EAX
                    {
                        return reinterpret_cast<int*>(mgs4e::mem::RipTarget(reinterpret_cast<uintptr_t>(q) + 2));
                    }
                }
            }
        }

        return nullptr;
    }

    // ---------------------- PIX programmatic GPU capture ----------------------
    // The game can't be launched under a capture tool (it exits when it isn't started by
    // its own launcher), and D3D12 can't be hooked after the device exists. But our ASI
    // runs before the renderer initialises, so we can load PIX's capture library ourselves
    // and trigger captures from inside the process - no launcher and no GUI needed.

    using PFN_CaptureNextFrame = HRESULT (WINAPI*)(PCWSTR, UINT32);
    PFN_CaptureNextFrame g_PixCaptureNextFrame = nullptr;

    bool LoadPixGpuCapturer()
    {
        if (GetModuleHandleW(L"WinPixGpuCapturer.dll") == nullptr)
        {
            wchar_t programFiles[MAX_PATH] {};
            if (GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH) == 0)
            {
                spdlog::error("MGS4: PIX capture: could not resolve %ProgramFiles%.");
                return false;
            }

            std::error_code ec;
            const std::filesystem::path root = std::filesystem::path(programFiles) / L"Microsoft PIX";

            // PIX installs into a versioned folder; take the newest one present.
            std::filesystem::path newest;
            if (std::filesystem::exists(root, ec))
            {
                for (const auto& entry : std::filesystem::directory_iterator(root, ec))
                {
                    if (!entry.is_directory(ec) || !std::filesystem::exists(entry.path() / L"WinPixGpuCapturer.dll", ec))
                    {
                        continue;
                    }
                    if (newest.empty() || entry.path().filename() > newest.filename())
                    {
                        newest = entry.path();
                    }
                }
            }

            if (newest.empty())
            {
                spdlog::error("MGS4: PIX capture: no WinPixGpuCapturer.dll under {}. Is PIX installed?", root.string());
                return false;
            }

            const std::filesystem::path capturer = newest / L"WinPixGpuCapturer.dll";
            if (LoadLibraryW(capturer.c_str()) == nullptr)
            {
                spdlog::error("MGS4: PIX capture: failed to load {} (error {}).", capturer.string(), GetLastError());
                return false;
            }

            spdlog::info("MGS4: PIX capture: loaded {}.", capturer.string());
        }

        const HMODULE pix = GetModuleHandleW(L"WinPixGpuCapturer.dll");
        g_PixCaptureNextFrame = reinterpret_cast<PFN_CaptureNextFrame>(GetProcAddress(pix, "CaptureNextFrame"));

        if (!g_PixCaptureNextFrame)
        {
            spdlog::error("MGS4: PIX capture: CaptureNextFrame not exported.");
            return false;
        }

        return true;
    }

    // Logs a disassembly of a range of mgs4.exe, live. The executable's .text is encrypted on
    // disk by the Steam DRM, so it can only be read meaningfully from a running process - and
    // reading it from inside the process avoids needing a memory dump and an external
    // disassembler agreeing on a base address.
    void LogDisassembly(uintptr_t rva, size_t length)
    {
        auto* const start = reinterpret_cast<uint8_t*>(mgs4e::game::Module()) + rva;

        if (!mgs4e::mem::Readable(start, length))
        {
            spdlog::error("MGS4: Disassembly: mgs4.exe+{:X} (+{} bytes) is not readable.", rva, length);
            return;
        }

        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

        spdlog::info("MGS4: ===== disassembly of mgs4.exe+{:X}, {} bytes =====", rva, length);

        size_t offset = 0;
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

        while (offset < length)
        {
            // A range that starts mid-instruction decodes as garbage until it resynchronises,
            // so skip bad bytes rather than giving up on the whole range.
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, start + offset, length - offset,
                &instruction, operands)))
            {
                offset++;
                continue;
            }

            char text[256] {};
            ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                instruction.operand_count_visible, text, sizeof(text),
                reinterpret_cast<ZyanU64>(start + offset), ZYAN_NULL);

            spdlog::info("  +{:X}  {}", rva + offset, text);

            offset += instruction.length;
        }

        spdlog::info("MGS4: ===== end disassembly ({} bytes decoded) =====", offset);
    }

    // Finds the code that converts a screen-space pixel coordinate into the UI's 16-bit
    // fixed-point vertex format.
    //
    // The static UI never needs this: it is authored in a virtual 1280x720 space and scaled
    // by "field * 1280 / bufferSize * 16", where the buffer size cancels out, so it is safe at
    // any resolution. The elements that break at high resolutions - the crosshair, the Solid
    // Eye overlays - are world positions projected to *screen* pixels, so their coordinate
    // grows with the render buffer and overflows once it passes 2047.94px.
    //
    // An earlier hunt looked for "SHL reg, 4" next to a render size read and found a single
    // false positive. The multiply by 16 is a float constant (mgs4.exe+1664D18 = 16.0f), not
    // a shift, so the fingerprint to look for is a function that reads the render size *and*
    // that constant.
    // Candidate conversion sites the scan turns up, kept so the trap probe can arm all of them.
    std::vector<uintptr_t> g_FixedPointCandidates;

    void ScanFixedPointSites()
    {
        auto* const base = reinterpret_cast<uint8_t*>(mgs4e::game::Module());
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto* sections = IMAGE_FIRST_SECTION(nt);

        uintptr_t textStart = 0;
        size_t textSize = 0;

        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
        {
            if (std::memcmp(sections[i].Name, ".text", 5) == 0)
            {
                textStart = sections[i].VirtualAddress;
                textSize = sections[i].Misc.VirtualSize;
                break;
            }
        }

        if (textStart == 0 || textSize == 0)
        {
            spdlog::error("MGS4: Fixed-point scan: could not locate .text.");
            return;
        }

        spdlog::info("MGS4: Fixed-point scan: .text at +{:X}, {} bytes.", textStart, textSize);

        // What a hit refers to, as a bitmask so clusters can be summarised in one value.
        constexpr uint32_t kHitSizeX = 1;
        constexpr uint32_t kHitSizeY = 2;
        constexpr uint32_t kHitConst16 = 4;

        struct Hit { uintptr_t rva; uint32_t kind; };
        std::vector<Hit> hits;

        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

        size_t offset = 0;
        while (offset < textSize)
        {
            uint8_t* const at = base + textStart + offset;

            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, at, textSize - offset,
                &instruction, operands)))
            {
                offset++;
                continue;
            }

            for (ZyanU8 op = 0; op < instruction.operand_count_visible; op++)
            {
                if (operands[op].type != ZYDIS_OPERAND_TYPE_MEMORY
                    || operands[op].mem.base != ZYDIS_REGISTER_RIP)
                {
                    continue;
                }

                const uintptr_t target = (textStart + offset + instruction.length)
                    + static_cast<uintptr_t>(operands[op].mem.disp.value);

                uint32_t kind = 0;
                if (target == kRvaRenderBufferSizeX) { kind = kHitSizeX; }
                else if (target == kRvaRenderBufferSizeY) { kind = kHitSizeY; }
                else if (target == 0x1664D18) { kind = kHitConst16; }

                if (kind != 0)
                {
                    hits.push_back({ textStart + offset, kind });
                }
            }

            offset += instruction.length;
        }

        spdlog::info("MGS4: Fixed-point scan: {} reference(s) found.", hits.size());

        // Report only where a render size read and the 16.0f constant appear close together.
        constexpr uintptr_t kWindow = 256;
        int clusters = 0;

        for (size_t i = 0; i < hits.size(); i++)
        {
            uint32_t combined = hits[i].kind;
            size_t j = i;

            while (j + 1 < hits.size() && hits[j + 1].rva - hits[i].rva <= kWindow)
            {
                j++;
                combined |= hits[j].kind;
            }

            const bool hasSize = (combined & (kHitSizeX | kHitSizeY)) != 0;
            const bool hasConst = (combined & kHitConst16) != 0;

            if (hasSize && hasConst)
            {
                clusters++;
                spdlog::info("MGS4: Fixed-point scan: cluster at +{:X}..+{:X}:", hits[i].rva, hits[j].rva);
                for (size_t k = i; k <= j; k++)
                {
                    const char* what = (hits[k].kind == kHitSizeX) ? "bufferSizeX"
                        : (hits[k].kind == kHitSizeY) ? "bufferSizeY" : "16.0f";
                    spdlog::info("    +{:X}  {}", hits[k].rva, what);
                }

                // The code itself decides it: the safe path *divides* by the render size, so
                // the resolution cancels. A conversion that leaves screen pixels in the value
                // multiplies instead, and lands in a 16-bit store.
                if (RenderPipeline::bScanClusterDisassembly)
                {
                    const uintptr_t from = hits[i].rva - 48;
                    LogDisassembly(from, static_cast<size_t>((hits[j].rva - from) + 96));
                }
            }

            i = j;
        }

        spdlog::info("MGS4: Fixed-point scan: {} cluster(s) read both a render size and 16.0f.", clusters);

        // Second pass: the truncation itself. A position reaches the vertex buffer as
        // R16G16_SINT, so somewhere a float is converted to an integer and stored 16 bits
        // wide - that store is where a coordinate past 2047.94px silently wraps.
        spdlog::info("MGS4: Fixed-point scan: looking for float-to-int16 stores...");

        uintptr_t lastConvert = 0;
        uintptr_t lastScale = 0;
        int stores = 0;
        offset = 0;

        while (offset < textSize)
        {
            uint8_t* const at = base + textStart + offset;

            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, at, textSize - offset,
                &instruction, operands)))
            {
                offset++;
                continue;
            }

            const uintptr_t rva = textStart + offset;

            // Track the fixed-point scale constants: 16.0f and its reciprocal, which sits
            // immediately before it. A conversion that is genuinely producing 1/16px fixed
            // point passes through one of them; the other 1500 float-to-int16 stores in the
            // binary do not.
            for (ZyanU8 op = 0; op < instruction.operand_count_visible; op++)
            {
                if (operands[op].type == ZYDIS_OPERAND_TYPE_MEMORY
                    && operands[op].mem.base == ZYDIS_REGISTER_RIP)
                {
                    const uintptr_t target = rva + instruction.length
                        + static_cast<uintptr_t>(operands[op].mem.disp.value);
                    if (target == 0x1664D18 || target == 0x1664D14)
                    {
                        lastScale = rva;
                    }
                }
            }

            if (instruction.mnemonic == ZYDIS_MNEMONIC_CVTTSS2SI
                || instruction.mnemonic == ZYDIS_MNEMONIC_CVTTPS2DQ
                || instruction.mnemonic == ZYDIS_MNEMONIC_CVTPS2DQ)
            {
                lastConvert = rva;
            }
            else if (lastConvert != 0 && rva - lastConvert <= 48
                && lastScale != 0 && rva - lastScale <= 96
                && instruction.operand_count_visible >= 2
                && operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY
                && operands[0].size == 16
                && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
            {
                stores++;
                spdlog::info("    +{:X}  16-bit store; convert at +{:X}, fixed-point scale at +{:X}",
                    rva, lastConvert, lastScale);
                lastConvert = 0;
            }

            offset += instruction.length;
        }

        spdlog::info("MGS4: Fixed-point scan: {} float-to-int16 store(s).", stores);

        // Third pass: packed stores.
        //
        // The second pass required a 16-bit-wide store operand, which structurally cannot match
        // the most natural way to emit a vertex - two coordinates packed into one 32-bit write,
        // or a whole quad written with SSE. Since every candidate the earlier passes produced
        // turned out to be dead or harmless, that exclusion is the likeliest place the real
        // conversion has been hiding.
        //
        // Two shapes are reported: an explicit 32->16 pack (packssdw / packusdw), and a wide
        // store that follows a float-to-int conversion with the fixed-point scale nearby.
        spdlog::info("MGS4: Fixed-point scan: looking for packed 16-bit stores...");

        lastConvert = 0;
        lastScale = 0;
        int packs = 0;
        int wideStores = 0;
        offset = 0;

        while (offset < textSize)
        {
            uint8_t* const at = base + textStart + offset;

            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, at, textSize - offset,
                &instruction, operands)))
            {
                offset++;
                continue;
            }

            const uintptr_t rva = textStart + offset;

            for (ZyanU8 op = 0; op < instruction.operand_count_visible; op++)
            {
                if (operands[op].type == ZYDIS_OPERAND_TYPE_MEMORY
                    && operands[op].mem.base == ZYDIS_REGISTER_RIP)
                {
                    const uintptr_t target = rva + instruction.length
                        + static_cast<uintptr_t>(operands[op].mem.disp.value);
                    if (target == 0x1664D18 || target == 0x1664D14)
                    {
                        lastScale = rva;
                    }
                }
            }

            if (instruction.mnemonic == ZYDIS_MNEMONIC_PACKSSDW
                || instruction.mnemonic == ZYDIS_MNEMONIC_PACKUSDW)
            {
                packs++;
                g_FixedPointCandidates.push_back(rva);
                spdlog::info("    +{:X}  {} (32->16 pack){}", rva,
                    ZydisMnemonicGetString(instruction.mnemonic),
                    (lastScale != 0 && rva - lastScale <= 256) ? "  <== fixed-point scale nearby" : "");
            }
            else if (instruction.mnemonic == ZYDIS_MNEMONIC_CVTTSS2SI
                || instruction.mnemonic == ZYDIS_MNEMONIC_CVTTPS2DQ
                || instruction.mnemonic == ZYDIS_MNEMONIC_CVTPS2DQ)
            {
                lastConvert = rva;
            }
            // Checking operand shape alone is not enough: CMP also has a memory destination
            // and a register source, and one slipped into the candidate list that way.
            else if (lastConvert != 0 && rva - lastConvert <= 48
                && lastScale != 0 && rva - lastScale <= 96
                && (instruction.mnemonic == ZYDIS_MNEMONIC_MOV
                    || instruction.mnemonic == ZYDIS_MNEMONIC_MOVSS
                    || instruction.mnemonic == ZYDIS_MNEMONIC_MOVD
                    || instruction.mnemonic == ZYDIS_MNEMONIC_MOVQ
                    || instruction.mnemonic == ZYDIS_MNEMONIC_MOVAPS
                    || instruction.mnemonic == ZYDIS_MNEMONIC_MOVUPS
                    || instruction.mnemonic == ZYDIS_MNEMONIC_MOVDQA
                    || instruction.mnemonic == ZYDIS_MNEMONIC_MOVDQU)
                && instruction.operand_count_visible >= 2
                && operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY
                && (operands[0].size == 32 || operands[0].size == 64 || operands[0].size == 128)
                && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
            {
                wideStores++;
                g_FixedPointCandidates.push_back(rva);
                spdlog::info("    +{:X}  {}-bit store; convert at +{:X}, scale at +{:X}",
                    rva, operands[0].size, lastConvert, lastScale);
                lastConvert = 0;
            }

            offset += instruction.length;
        }

        spdlog::info("MGS4: Fixed-point scan: {} pack(s), {} wide store(s) after a conversion.",
            packs, wideStores);

        // Fourth pass: integer narrowing.
        //
        // Every pass so far required a float-to-int conversion, which is why none of them found
        // it. By the time a UI position reaches the vertex buffer it is already an int32 in
        // 1/16px units sitting in a struct - the coordinates written at +529FDE and +F70C12 and
        // read back at +F72F50 live at [obj+0x1C] and [obj+0x20]. Packing that into an
        // R16G16_SINT vertex is pure integer narrowing:
        //
        //     mov eax, [rsi+0x1C]
        //     mov [rdi], ax          <- the wrap, with no float anywhere near it
        //
        // So: a 16-bit store whose value came from a 32-bit load at one of those offsets.
        spdlog::info("MGS4: Fixed-point scan: looking for integer narrowing from [reg+1C]/[reg+20]...");

        std::map<ZydisRegister, uintptr_t> lastFieldLoad;
        int narrowing = 0;
        offset = 0;

        while (offset < textSize)
        {
            uint8_t* const at = base + textStart + offset;

            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, at, textSize - offset,
                &instruction, operands)))
            {
                offset++;
                continue;
            }

            const uintptr_t rva = textStart + offset;

            // A 32-bit load out of the coordinate fields. Require a plain [base+disp] form:
            // an indexed access like [r14+rcx*8+0x20] is an array walk that happens to share
            // the displacement, not the struct field we care about.
            const bool isFieldLoad = instruction.mnemonic == ZYDIS_MNEMONIC_MOV
                && instruction.operand_count_visible >= 2
                && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER
                && operands[0].size == 32
                && operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY
                && operands[1].mem.index == ZYDIS_REGISTER_NONE
                && operands[1].mem.base != ZYDIS_REGISTER_NONE
                && (operands[1].mem.disp.value == 0x1C || operands[1].mem.disp.value == 0x20);

            if (isFieldLoad)
            {
                lastFieldLoad[ZydisRegisterGetLargestEnclosing(
                    ZYDIS_MACHINE_MODE_LONG_64, operands[0].reg.value)] = rva;
            }
            // ...and a 16-bit store of that same register soon after.
            else if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV
                && instruction.operand_count_visible >= 2
                && operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY
                && operands[0].size == 16
                && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
            {
                const ZydisRegister full = ZydisRegisterGetLargestEnclosing(
                    ZYDIS_MACHINE_MODE_LONG_64, operands[1].reg.value);
                const auto found = lastFieldLoad.find(full);

                if (found != lastFieldLoad.end() && (rva - found->second) <= 64)
                {
                    narrowing++;
                    g_FixedPointCandidates.push_back(rva);
                    spdlog::info("    +{:X}  16-bit store of a value loaded at +{:X}",
                        rva, found->second);
                }
            }

            // Forget any register this instruction overwrites. Without it, a load into EAX
            // followed by anything else writing EAX still counted as "the field is in EAX" -
            // which is how a struct initialiser storing the literal 0x3E38 ended up looking
            // like a coordinate narrowing.
            if (!isFieldLoad)
            {
                if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL)
                {
                    lastFieldLoad.clear();   // volatile registers are gone across a call
                }
                else
                {
                    for (ZyanU8 op = 0; op < instruction.operand_count_visible; op++)
                    {
                        if (operands[op].type == ZYDIS_OPERAND_TYPE_REGISTER
                            && (operands[op].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0)
                        {
                            lastFieldLoad.erase(ZydisRegisterGetLargestEnclosing(
                                ZYDIS_MACHINE_MODE_LONG_64, operands[op].reg.value));
                        }
                    }
                }
            }

            offset += instruction.length;
        }

        spdlog::info("MGS4: Fixed-point scan: {} integer narrowing store(s).", narrowing);

        // Fifth pass: the wrap in the arithmetic itself.
        //
        // Every pass so far assumed the value is computed at full width and then truncated, so
        // it looked for the truncation. But it can happen in the computation - mgs4.exe+4E160B
        // does exactly this:
        //
        //     imul ecx, eax      ; 32-bit
        //     shl  cx, 0x04      ; 16-bit: the multiply by 16 that overflows
        //     mov  [rdx], cx
        //
        // There is no conversion, no scale constant and no narrowing load; the wrap is in the
        // operand size. Nothing above could have matched it.
        spdlog::info("MGS4: Fixed-point scan: looking for 16-bit arithmetic feeding a store...");

        std::map<ZydisRegister, uintptr_t> lastNarrowMath;
        int narrowMath = 0;
        offset = 0;

        while (offset < textSize)
        {
            uint8_t* const at = base + textStart + offset;

            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, at, textSize - offset,
                &instruction, operands)))
            {
                offset++;
                continue;
            }

            const uintptr_t rva = textStart + offset;

            // Specifically a multiply by 16 done at 16 bits - the fixed-point conversion, not
            // arithmetic in general. Any 16-bit add or shift feeding a 16-bit store matches
            // over a thousand times, because that is just ordinary short-field handling; the
            // scale factor is what makes it a coordinate conversion.
            const bool isNarrowMath =
                ((instruction.mnemonic == ZYDIS_MNEMONIC_SHL
                    && instruction.operand_count_visible >= 2
                    && operands[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE
                    && operands[1].imm.value.u == 4)
                 || (instruction.mnemonic == ZYDIS_MNEMONIC_IMUL
                    && instruction.operand_count_visible >= 3
                    && operands[2].type == ZYDIS_OPERAND_TYPE_IMMEDIATE
                    && operands[2].imm.value.u == 16))
                && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER
                && operands[0].size == 16;

            if (isNarrowMath)
            {
                lastNarrowMath[ZydisRegisterGetLargestEnclosing(
                    ZYDIS_MACHINE_MODE_LONG_64, operands[0].reg.value)] = rva;
            }
            else if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV
                && instruction.operand_count_visible >= 2
                && operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY
                && operands[0].size == 16
                && operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
            {
                const ZydisRegister full = ZydisRegisterGetLargestEnclosing(
                    ZYDIS_MACHINE_MODE_LONG_64, operands[1].reg.value);
                const auto found = lastNarrowMath.find(full);

                if (found != lastNarrowMath.end() && (rva - found->second) <= 48)
                {
                    narrowMath++;
                    g_FixedPointCandidates.push_back(rva);
                    spdlog::info("    +{:X}  16-bit store; 16-bit arithmetic at +{:X}",
                        rva, found->second);
                }
            }

            if (!isNarrowMath)
            {
                if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL)
                {
                    lastNarrowMath.clear();
                }
                else
                {
                    for (ZyanU8 op = 0; op < instruction.operand_count_visible; op++)
                    {
                        if (operands[op].type == ZYDIS_OPERAND_TYPE_REGISTER
                            && (operands[op].actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) != 0)
                        {
                            lastNarrowMath.erase(ZydisRegisterGetLargestEnclosing(
                                ZYDIS_MACHINE_MODE_LONG_64, operands[op].reg.value));
                        }
                    }
                }
            }

            offset += instruction.length;
        }

        spdlog::info("MGS4: Fixed-point scan: {} store(s) fed by 16-bit arithmetic.", narrowMath);

        // Sixth pass: truncation on the way *in*, not on the way out.
        //
        // Every pass so far hunted a truncating store, assuming a wide value is narrowed as it
        // is written. But the coordinates are *stored* as int32 - +529FDE, +F70C12 and +4B62AC
        // all do exactly that - and no store in the binary truncates them. So the truncation
        // has to be a 16-bit read of a 32-bit field:
        //
        //     movsx eax, word ptr [obj+0x1C]   ; field holds 61440, read as 16 bits -> -4096
        //
        // which is how 3840px at 1/16 becomes -256px, the value measured in the vertex data.
        // No store-based fingerprint could match this.
        spdlog::info("MGS4: Fixed-point scan: looking for 16-bit reads of the coordinate fields...");

        int narrowLoads = 0;
        offset = 0;

        while (offset < textSize)
        {
            uint8_t* const at = base + textStart + offset;

            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, at, textSize - offset,
                &instruction, operands)))
            {
                offset++;
                continue;
            }

            const uintptr_t rva = textStart + offset;

            if ((instruction.mnemonic == ZYDIS_MNEMONIC_MOVSX
                    || instruction.mnemonic == ZYDIS_MNEMONIC_MOVZX
                    || instruction.mnemonic == ZYDIS_MNEMONIC_MOV)
                && instruction.operand_count_visible >= 2
                && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER
                && operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY
                && operands[1].size == 16
                && operands[1].mem.index == ZYDIS_REGISTER_NONE
                && operands[1].mem.base != ZYDIS_REGISTER_NONE
                && (operands[1].mem.disp.value == 0x1C || operands[1].mem.disp.value == 0x20))
            {
                narrowLoads++;
                g_FixedPointCandidates.push_back(rva);
                spdlog::info("    +{:X}  16-bit read of [{}+{:X}]", rva,
                    ZydisRegisterGetString(operands[1].mem.base),
                    static_cast<int>(operands[1].mem.disp.value));
            }

            offset += instruction.length;
        }

        spdlog::info("MGS4: Fixed-point scan: {} 16-bit read(s) of the coordinate fields.", narrowLoads);
    }

    // mgs4.exe+4F9F5A converts four floats to 1/16px fixed point and stores them as int16 at
    // [rbx+0x47C..0x482], with no normalising divide anywhere - the shape of the conversion
    // that loses dynamic HUD positions at high resolutions.
    //
    // Whether it actually does depends on what units those floats carry. Screen pixels wrap
    // once they pass 2047.94; virtual 1280x720 coordinates never can. Logging them settles it:
    // values in the thousands at a 7680-wide buffer mean screen space and this is the site.
    // Every float-to-int16 store the scan turned up. Hooking all of them at once answers, in
    // one session, both which are live during gameplay and what magnitude they carry - a value
    // whose 1/16px encoding is near or past +-32767 is a coordinate about to be destroyed.
    constexpr uintptr_t kFixedPointStores[] = {
        0x44C733, 0x4B63FD, 0x4F9F96, 0x4F9FA7, 0x4F9FB6, 0x5013A6, 0x5056AA, 0x5056BB,
        0x5056CA, 0x5190F5, 0x51B6A5, 0x7437F4, 0xC13C83, 0xC2647D,
        0xD1DCBC   // known-safe (it normalises) - included as a control that the probe works
    };

    std::vector<safetyhook::MidHook> g_FixedPointProbeHooks;
    std::atomic<int> g_FixedPointProbeBudget { 0 };

    // Resolved at install time by decoding each store, so the hook knows which register holds
    // the value being written.
    std::array<ZydisRegister, std::size(kFixedPointStores)> g_FixedPointStoreRegs {};

    uint64_t ReadContextRegister(const safetyhook::Context64& ctx, ZydisRegister reg)
    {
        switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg))
        {
        case ZYDIS_REGISTER_RAX: return ctx.rax;
        case ZYDIS_REGISTER_RBX: return ctx.rbx;
        case ZYDIS_REGISTER_RCX: return ctx.rcx;
        case ZYDIS_REGISTER_RDX: return ctx.rdx;
        case ZYDIS_REGISTER_RSI: return ctx.rsi;
        case ZYDIS_REGISTER_RDI: return ctx.rdi;
        case ZYDIS_REGISTER_RBP: return ctx.rbp;
        case ZYDIS_REGISTER_RSP: return ctx.rsp;
        case ZYDIS_REGISTER_R8:  return ctx.r8;
        case ZYDIS_REGISTER_R9:  return ctx.r9;
        case ZYDIS_REGISTER_R10: return ctx.r10;
        case ZYDIS_REGISTER_R11: return ctx.r11;
        case ZYDIS_REGISTER_R12: return ctx.r12;
        case ZYDIS_REGISTER_R13: return ctx.r13;
        case ZYDIS_REGISTER_R14: return ctx.r14;
        case ZYDIS_REGISTER_R15: return ctx.r15;
        default: return 0;
        }
    }

    // ---------------- Hardware breakpoint probe ----------------
    // Mid hooks rewrite the instruction, which is unusable here: several of these stores are
    // three bytes and cannot hold a jump, and hooking them crashed the game. Debug registers
    // watch an address without touching it, so any instruction can be observed safely.
    //
    // Four breakpoints exist per thread, so the sites are armed in groups of four via config.
    // An execute breakpoint fires *before* the instruction runs, so the context holds the
    // value about to be stored - which shows whether the 16-bit store is about to destroy it.

    struct BreakpointSite
    {
        uintptr_t rva;
        ZydisRegister valueReg;
        ZydisRegister baseReg;
        int64_t displacement;

        // Data watchpoints instead of execute breakpoints: set on an address holding an
        // already-wrapped coordinate, so whatever writes it announces itself.
        bool isWatch;
        uintptr_t watchAddress;

        // Render size watches log each distinct caller once rather than every hit, since the
        // globals are read constantly and the interesting output is the set of consumers.
        bool isRenderSizeWatch;

        // Break on writes only, rather than reads and writes.
        //
        // A read-or-write watch on the anchor fires on our own diagnostic reads - the dump
        // memcpys the same address every frame - so every hit reported our own ASI as
        // the accessor and drowned out the actual producer. Anything the diagnostics themselves
        // read needs this.
        bool writeOnly = false;
    };

    std::array<BreakpointSite, 4> g_BreakpointSites {};
    int g_BreakpointCount = 0;
    void* g_VehHandle = nullptr;

    // Every distinct instruction that touches the render size globals, collected at runtime.
    // The static scan found 818 references across 162 functions; this reports only the ones
    // that actually execute, which is a far smaller and more useful set - and it covers load
    // time, when the UI geometry is built and the coordinate we care about is produced.
    std::set<uintptr_t> g_RenderSizeReaders;
    std::mutex g_RenderSizeReadersMutex;

    // ---------------- Software breakpoint probe ----------------
    // Debug registers gave four watchpoints, which meant walking 189 candidate sites four at a
    // time. An INT3 costs one byte at a known instruction boundary, so every candidate can be
    // armed at once - and it is safer than the mid hooks that crashed the game, which needed
    // five bytes and silently ate whatever followed a short instruction.
    //
    // On a hit: log it, restore the original byte, rewind RIP so the real instruction runs, and
    // single-step once to re-arm behind it. After a few hits a site is left disarmed, so a
    // breakpoint in a hot path stops costing anything.
    struct TrapSite
    {
        uintptr_t rva;
        uint8_t original;
        bool armed;
        int hits;

        // For a 16-bit store, the register holding the value. Worth logging in full: an
        // instruction like "shl cx,4" only touches the low half, so the upper 16 bits of ECX
        // still carry the untruncated product - printing both shows the wrap as it happens.
        ZydisRegister valueReg;

        // For a 16-bit *read* of a field, where to look. Reading the full 32 bits at that
        // address shows whether reading it as a word loses anything - which is the wrap, if
        // the field holds a coordinate too large for 16 bits.
        ZydisRegister baseReg;
        int64_t displacement;
        bool isLoad;
    };

    std::vector<TrapSite> g_TrapSites;
    std::mutex g_TrapMutex;
    std::atomic<bool> g_TrapActive { false };
    constexpr int kTrapHitsPerSite = 4;

    // Which site this thread must re-arm after its single step. Per-thread, because several
    // threads can be mid-step on different sites at once.
    thread_local int t_PendingRearm = -1;

    // Which guarded upload buffer this thread must re-protect after stepping over a read.
    thread_local int t_PendingUploadRearm = -1;

    LONG CALLBACK BreakpointHandler(EXCEPTION_POINTERS* info);

    bool WriteByteAt(uintptr_t address, uint8_t value)
    {
        DWORD previous = 0;
        auto* const target = reinterpret_cast<void*>(address);
        if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &previous))
        {
            return false;
        }
        *reinterpret_cast<uint8_t*>(address) = value;
        VirtualProtect(target, 1, previous, &previous);
        FlushInstructionCache(GetCurrentProcess(), target, 1);
        return true;
    }

    void ArmTrapSites()
    {
        // Two passes, and the order matters. Writing the INT3 before the site is in the list
        // leaves a window where a thread can execute a breakpoint the handler cannot identify,
        // which falls through to the game's own crash handler - that is exactly how the first
        // attempt died, at +42785C. Build the whole list first, then arm.
        //
        // The list is also built once and never resized afterwards, so the handler can search
        // it without taking a lock. Locking inside an exception handler is its own hazard.
        if (g_TrapSites.empty())
        {
            g_TrapSites.reserve(g_FixedPointCandidates.size());

            for (const uintptr_t rva : g_FixedPointCandidates)
            {
                const uintptr_t address = reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + rva;
                if (!mgs4e::mem::Readable(reinterpret_cast<void*>(address), 1))
                {
                    continue;
                }

                const uint8_t original = *reinterpret_cast<uint8_t*>(address);
                if (original == 0xCC)
                {
                    continue;   // already a breakpoint; leave it alone
                }

                // Decode the site so the handler knows what to report: the register holding a
                // stored value, or the address a 16-bit read is about to take a value from.
                ZydisRegister valueReg = ZYDIS_REGISTER_NONE;
                ZydisRegister baseReg = ZYDIS_REGISTER_NONE;
                int64_t displacement = 0;
                bool isLoad = false;
                {
                    ZydisDecoder decoder;
                    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
                    ZydisDecodedInstruction instruction;
                    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

                    if (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder,
                        reinterpret_cast<void*>(address), 16, &instruction, operands))
                        && instruction.operand_count_visible >= 2)
                    {
                        if (operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
                        {
                            valueReg = operands[1].reg.value;
                        }
                        else if (operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY
                            && operands[1].size == 16)
                        {
                            isLoad = true;
                            baseReg = operands[1].mem.base;
                            displacement = operands[1].mem.disp.value;
                        }
                    }
                }

                g_TrapSites.push_back(TrapSite {
                    rva, original, false, 0, valueReg, baseReg, displacement, isLoad });
            }
        }

        for (auto& site : g_TrapSites)
        {
            site.hits = 0;
            site.armed = false;
        }

        // Put our handler back at the front of the vectored chain. The game installs its own
        // crash reporter after the ASI loads, and whoever registered last with First=1 runs
        // first - which is why the first attempt never saw its own breakpoints and the game
        // reported them as fatal instead.
        if (g_VehHandle)
        {
            RemoveVectoredExceptionHandler(g_VehHandle);
        }
        g_VehHandle = AddVectoredExceptionHandler(1, BreakpointHandler);
        spdlog::info("MGS4: Trap: exception handler re-registered at the front of the chain ({}).",
            g_VehHandle ? "ok" : "FAILED");

        // Live from here: the handler will recognise anything it sees from now on.
        g_TrapActive.store(true);

        int armed = 0;
        for (auto& site : g_TrapSites)
        {
            if (WriteByteAt(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + site.rva, 0xCC))
            {
                site.armed = true;
                armed++;
            }
        }

        spdlog::info("MGS4: Trap: armed {} of {} candidate site(s).", armed, g_TrapSites.size());
    }

    void DisarmTrapSites()
    {
        std::lock_guard lock(g_TrapMutex);
        g_TrapActive.store(false);

        int restored = 0;
        for (auto& site : g_TrapSites)
        {
            if (site.armed)
            {
                WriteByteAt(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + site.rva, site.original);
                site.armed = false;
                restored++;
            }
        }
        spdlog::info("MGS4: Trap: disarmed {} site(s).", restored);
    }

    // State for the PAGE_GUARD probe (see ArmGuardOn).
    struct GuardRecord { uintptr_t address; uintptr_t rip; };

    std::vector<GuardRecord> g_GuardLog;
    std::atomic<size_t> g_GuardLogIndex { 0 };
    std::atomic<bool> g_GuardRecording { false };

    uintptr_t g_GuardedBase = 0;
    uintptr_t g_GuardWatchedValue = 0;
    size_t g_GuardedSize = 0;
    std::atomic<int> g_GuardBudget { 0 };
    std::set<uintptr_t> g_GuardWriters;
    std::mutex g_GuardWritersMutex;
    // Per-site, not shared: a site in a hot loop would otherwise consume the whole budget and
    // make the other three look silent when they were simply starved.
    std::array<std::atomic<int>, 4> g_BreakpointBudget {};

    uint64_t ReadWin32Register(const CONTEXT& ctx, ZydisRegister reg)
    {
        switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg))
        {
        case ZYDIS_REGISTER_RAX: return ctx.Rax;
        case ZYDIS_REGISTER_RBX: return ctx.Rbx;
        case ZYDIS_REGISTER_RCX: return ctx.Rcx;
        case ZYDIS_REGISTER_RDX: return ctx.Rdx;
        case ZYDIS_REGISTER_RSI: return ctx.Rsi;
        case ZYDIS_REGISTER_RDI: return ctx.Rdi;
        case ZYDIS_REGISTER_RBP: return ctx.Rbp;
        case ZYDIS_REGISTER_RSP: return ctx.Rsp;
        case ZYDIS_REGISTER_R8:  return ctx.R8;
        case ZYDIS_REGISTER_R9:  return ctx.R9;
        case ZYDIS_REGISTER_R10: return ctx.R10;
        case ZYDIS_REGISTER_R11: return ctx.R11;
        case ZYDIS_REGISTER_R12: return ctx.R12;
        case ZYDIS_REGISTER_R13: return ctx.R13;
        case ZYDIS_REGISTER_R14: return ctx.R14;
        case ZYDIS_REGISTER_R15: return ctx.R15;
        default: return 0;
        }
    }

    LONG CALLBACK BreakpointHandler(EXCEPTION_POINTERS* info)
    {
        CONTEXT* const context = info->ContextRecord;

        // An INT3 we planted. RIP is already past it.
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT && g_TrapActive.load())
        {
            // Use ExceptionAddress, which is documented as the address of the breakpoint
            // instruction. Deriving it as Rip-1 assumes RIP has advanced past the INT3, and
            // here it has not - RIP is the INT3 itself, so that lookup was always one byte off
            // and never matched, which is what sent these into the game's crash handler.
            const auto hit = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
            const uintptr_t rva = hit - reinterpret_cast<uintptr_t>(mgs4e::game::Module());

            // No lock: the site list is fixed once built, and only the flags inside it change.
            for (size_t i = 0; i < g_TrapSites.size(); i++)
            {
                auto& site = g_TrapSites[i];
                if (site.rva != rva || !site.armed)
                {
                    continue;
                }

                site.hits++;
                if (site.hits <= 2)
                {
                    if (site.isLoad && site.baseReg != ZYDIS_REGISTER_NONE)
                    {
                        const uint64_t base64 = ReadWin32Register(*context, site.baseReg);
                        const auto address = static_cast<uintptr_t>(
                            base64 + static_cast<uint64_t>(site.displacement));

                        if (mgs4e::mem::Readable(reinterpret_cast<void*>(address), 4))
                        {
                            int32_t full {};
                            std::memcpy(&full, reinterpret_cast<void*>(address), sizeof(full));
                            const auto asWord = static_cast<int16_t>(full & 0xFFFF);

                            // Only report values that could actually be a wrapped coordinate:
                            // past what int16 holds, but still a plausible screen position
                            // (up to 16384px at 1/16). Comparing 32 bits against 16 on its own
                            // flags every genuinely-16-bit field, because the neighbouring two
                            // bytes are a different field - which is what produced 68 bogus
                            // hits with values in the tens of millions.
                            if (full > 32767 && full < 262144)
                            {
                                spdlog::info("MGS4: TRAP +{:X} reads field = {} ({:.1f}px); as 16-bit {} ({:.1f}px)   <== WRAPS",
                                    rva, full, full / 16.0, asWord, asWord / 16.0);
                            }
                        }
                    }
                    else if (site.valueReg != ZYDIS_REGISTER_NONE)
                    {
                        const uint64_t raw = ReadWin32Register(*context, site.valueReg);
                        const auto full = static_cast<int32_t>(raw & 0xFFFFFFFF);
                        const auto stored = static_cast<int16_t>(raw & 0xFFFF);

                        spdlog::info("MGS4: TRAP +{:X} stores {} ({:.1f}px); full register {} ({:.1f}px){}",
                            rva, stored, stored / 16.0, full, full / 16.0,
                            (full != stored) ? "   <== TRUNCATED" : "");
                    }
                    else
                    {
                        spdlog::info("MGS4: TRAP +{:X} executed (hit {})", rva, site.hits);
                    }
                }

                // Put the real instruction back and rewind onto it.
                WriteByteAt(hit, site.original);
                site.armed = false;
                context->Rip = hit;

                // Re-arm behind the instruction, unless this site has told us enough already -
                // leaving a hot site disarmed keeps the cost bounded.
                if (site.hits < kTrapHitsPerSite)
                {
                    t_PendingRearm = static_cast<int>(i);
                    context->EFlags |= 0x100;   // trap flag: break after the next instruction
                }

                return EXCEPTION_CONTINUE_EXECUTION;
            }

            // Reached only if we were called but did not recognise the address - worth knowing,
            // because it distinguishes "our handler never ran" from "it ran and missed".
            static std::atomic<int> unknown { 0 };
            if (unknown.fetch_add(1) < 4)
            {
                spdlog::warn("MGS4: Trap: unrecognised INT3 at mgs4.exe+{:X} - passing it on.", rva);
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }

        // The single step after a read of a guarded upload buffer, used to put the guard back.
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP
            && t_PendingUploadRearm >= 0)
        {
            const int index = t_PendingUploadRearm;
            t_PendingUploadRearm = -1;

            std::lock_guard lock(g_UploadGuardMutex);
            if (index < static_cast<int>(g_UploadGuards.size()))
            {
                auto& guard = g_UploadGuards[index];
                DWORD previous = 0;
                if (guard.guarded && VirtualProtect(reinterpret_cast<void*>(guard.base),
                    guard.size, PAGE_READWRITE | PAGE_GUARD, &previous))
                {
                    // still guarded
                }
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        // The single step that follows a trap hit, used to put the INT3 back.
        if (info->ExceptionRecord->ExceptionCode == EXCEPTION_SINGLE_STEP && t_PendingRearm >= 0)
        {
            const int index = t_PendingRearm;
            t_PendingRearm = -1;

            if (g_TrapActive.load() && index < static_cast<int>(g_TrapSites.size()))
            {
                auto& site = g_TrapSites[index];
                if (!site.armed && WriteByteAt(reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + site.rva, 0xCC))
                {
                    site.armed = true;
                }
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (info->ExceptionRecord->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION)
        {
            // ExceptionInformation[0] is 0 for a read and 1 for a write; [1] is the address.
            const bool isWrite = info->ExceptionRecord->ExceptionInformation[0] == 1;
            const auto address = static_cast<uintptr_t>(info->ExceptionRecord->ExceptionInformation[1]);
            const uintptr_t rip = info->ContextRecord->Rip;
            const uintptr_t rva = rip - reinterpret_cast<uintptr_t>(mgs4e::game::Module());

            // An upload buffer we guarded at creation.
            //
            // PAGE_GUARD fires on any access and clears itself, so a read consumes the guard
            // and the write we actually care about then goes unseen - which is why the first
            // attempt reported 43 buffers guarded and zero writes. On a read, step over the
            // instruction and put the guard back; only a write is recorded and released.
            {
                std::lock_guard lock(g_UploadGuardMutex);
                for (size_t i = 0; i < g_UploadGuards.size(); i++)
                {
                    auto& guard = g_UploadGuards[i];
                    if (!guard.guarded || address < guard.base || address >= guard.base + guard.size)
                    {
                        continue;
                    }

                    if (isWrite)
                    {
                        guard.writer = rip - reinterpret_cast<uintptr_t>(mgs4e::game::Module());
                        guard.guarded = false;
                    }
                    else
                    {
                        g_UploadGuardReads.fetch_add(1);
                        t_PendingUploadRearm = static_cast<int>(i);
                        context->EFlags |= 0x100;
                    }

                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            }

            // Recording mode: store and move on. Logging here would be far too slow - the
            // whole heap is guarded, so this fires constantly.
            if (isWrite && g_GuardRecording.load())
            {
                const size_t slot = g_GuardLogIndex.fetch_add(1);
                if (slot < g_GuardLog.size())
                {
                    g_GuardLog[slot] = { address, rip };
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }

            if (isWrite && g_GuardBudget.fetch_sub(1) > 0)
            {
                bool isNew = false;
                {
                    std::lock_guard lock(g_GuardWritersMutex);
                    isNew = g_GuardWriters.insert(rip).second;
                }

                if (isNew)
                {
                    if (rva > 0x241BE000)
                    {
                        spdlog::info("MGS4: GUARD write to 0x{:X} from outside mgs4.exe (rip 0x{:X})",
                            address, rip);
                    }
                    else
                    {
                        spdlog::info("MGS4: GUARD write to 0x{:X} ({:+d} bytes from the wrapped value) by mgs4.exe+{:X}",
                            address, static_cast<int64_t>(address) - static_cast<int64_t>(g_GuardWatchedValue), rva);
                    }
                }
            }

            return EXCEPTION_CONTINUE_EXECUTION;
        }

        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        CONTEXT* const ctx = info->ContextRecord;
        const auto which = static_cast<int>(ctx->Dr6 & 0xF);

        for (int i = 0; i < g_BreakpointCount; i++)
        {
            if ((which & (1 << i)) == 0)
            {
                continue;
            }

            if (g_BreakpointBudget[i].fetch_sub(1) > 0)
            {
                const auto& site = g_BreakpointSites[i];

                if (site.isRenderSizeWatch)
                {
                    const uintptr_t caller = ctx->Rip - reinterpret_cast<uintptr_t>(mgs4e::game::Module());

                    bool isNew = false;
                    {
                        std::lock_guard lock(g_RenderSizeReadersMutex);
                        isNew = g_RenderSizeReaders.insert(caller).second;
                    }

                    if (isNew)
                    {
                        int32_t value {};
                        std::memcpy(&value, reinterpret_cast<void*>(site.watchAddress), sizeof(value));

                        // Anything past the image is not the game - almost always the graphics
                        // driver reading a buffer it was handed.
                        if (caller > 0x241BE000)
                        {
                            spdlog::info("MGS4: RenderSize read from OUTSIDE mgs4.exe (rip 0x{:X}) - driver, ignore",
                                ctx->Rip);
                        }
                        else
                        {
                            spdlog::info("MGS4: RenderSize read by mgs4.exe+{:X}  (value {})", caller, value);
                        }
                    }
                    continue;
                }

                if (site.isWatch)
                {
                    // A data breakpoint reports after the access, so Rip is the instruction
                    // following the one that touched it.
                    int16_t now {};
                    std::memcpy(&now, reinterpret_cast<void*>(site.watchAddress), sizeof(now));

                    // Name the module Rip is actually in. Reporting it as mgs4.exe+X
                    // unconditionally produced a 3.7GB "RVA" - the accessor is below the image
                    // base, so the subtraction wrapped. The writer here is a CRT memcpy called
                    // by the engine, so the interesting address is not Rip at all but the
                    // first frame back inside mgs4.exe.
                    HMODULE owner = nullptr;
                    char moduleName[MAX_PATH] = "?";
                    if (GetModuleHandleExA(
                            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(ctx->Rip), &owner) && owner)
                    {
                        GetModuleBaseNameA(GetCurrentProcess(), owner, moduleName,
                            sizeof(moduleName));
                    }

                    const uintptr_t offset = owner
                        ? ctx->Rip - reinterpret_cast<uintptr_t>(owner) : 0;

                    // The anchor is a float, so report it as one as well as an int16. Printing
                    // only the int16 showed "0" for the zero-fill and would have shown noise
                    // for the value that matters.
                    float asFloat {};
                    std::memcpy(&asFloat, reinterpret_cast<void*>(site.watchAddress),
                        sizeof(asFloat));

                    // One line per distinct writer. A watch on a per-frame buffer fires dozens
                    // of times from the same memcpy, and the interesting thing is the set of
                    // call sites, not the count.
                    const std::string callers = DescribeGameCallers();

                    static std::set<std::string> seenWriters;
                    static std::mutex writerMutex;
                    bool isNew = false;
                    {
                        std::lock_guard lock(writerMutex);
                        isNew = seenWriters.size() < 24
                            && seenWriters.insert(callers + std::to_string(asFloat)).second;
                    }

                    if (isNew)
                    {
                        spdlog::info("MGS4: WATCH 0x{:X} = {:.5f}f / int16 {}  by {}+{:X}{}",
                            site.watchAddress, asFloat, now, moduleName, offset, callers);
                    }
                    continue;
                }

                const uint64_t raw = ReadWin32Register(*ctx, site.valueReg);
                const auto stored = static_cast<int16_t>(raw & 0xFFFF);
                const auto full = static_cast<int32_t>(raw & 0xFFFFFFFF);
                const uint64_t base = (site.baseReg == ZYDIS_REGISTER_NONE)
                    ? 0 : ReadWin32Register(*ctx, site.baseReg);

                spdlog::info("MGS4: BP +{:X}  value {} ({:.1f}px)  stored as int16 {} ({:.1f}px){}  dest 0x{:X}",
                    site.rva, full, full / 16.0, stored, stored / 16.0,
                    (full != stored) ? "  <== WRAPPED" : "",
                    base + static_cast<uint64_t>(site.displacement));
            }
        }

        // Clear the status bits and set RF so the instruction executes once without
        // immediately re-triggering the same breakpoint.
        ctx->Dr6 = 0;
        ctx->EFlags |= 0x10000;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Debug registers are per-thread, so every thread in the process has to be armed - and
    // again periodically, since the engine creates worker threads as it goes.
    void ArmBreakpointsOnAllThreads()
    {
        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return;
        }

        THREADENTRY32 entry {};
        entry.dwSize = sizeof(entry);

        const DWORD self = GetCurrentThreadId();
        const DWORD process = GetCurrentProcessId();

        if (Thread32First(snapshot, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                {
                    continue;
                }

                const HANDLE thread = OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
                    FALSE, entry.th32ThreadID);
                if (!thread)
                {
                    continue;
                }

                SuspendThread(thread);

                CONTEXT ctx {};
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (GetThreadContext(thread, &ctx))
                {
                    uint64_t dr7 = 0;
                    for (int i = 0; i < g_BreakpointCount; i++)
                    {
                        const uintptr_t address =
                            reinterpret_cast<uintptr_t>(mgs4e::game::Module()) + g_BreakpointSites[i].rva;

                        const uintptr_t target = g_BreakpointSites[i].isWatch
                            ? g_BreakpointSites[i].watchAddress : address;

                        switch (i)
                        {
                        case 0: ctx.Dr0 = target; break;
                        case 1: ctx.Dr1 = target; break;
                        case 2: ctx.Dr2 = target; break;
                        case 3: ctx.Dr3 = target; break;
                        default: break;
                        }

                        // Local enable for slot i. Execute breakpoints take type 00 / length
                        // 00, which are the zero defaults; a data write watch needs type 01
                        // and length 01 (two bytes, matching one int16 coordinate).
                        dr7 |= (1ull << (i * 2));
                        if (g_BreakpointSites[i].isWatch)
                        {
                            // Type 11 breaks on read or write; type 01 on writes only.
                            //
                            // Read-or-write is right for a value written once and then only
                            // read, where the reader is the reachable end of the same code
                            // path. It is wrong for anything the diagnostics read themselves:
                            // on the anchor it fired on our own per-frame dump and reported
                            // our own ASI as the accessor every time.
                            dr7 |= ((g_BreakpointSites[i].writeOnly ? 0b01ull : 0b11ull)
                                << (16 + (i * 4)));

                            // Length: two bytes for a single int16 coordinate, four for the
                            // render size globals, which are int32.
                            dr7 |= ((g_BreakpointSites[i].isRenderSizeWatch ? 0b11ull : 0b01ull)
                                << (18 + (i * 4)));
                        }
                    }

                    ctx.Dr7 = dr7;
                    ctx.Dr6 = 0;
                    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                    SetThreadContext(thread, &ctx);
                }

                ResumeThread(thread);
                CloseHandle(thread);
            } while (Thread32Next(snapshot, &entry));
        }

        CloseHandle(snapshot);
    }

    // Finds memory holding an already-wrapped centre-screen coordinate and puts a write watch
    // on it. The wrapped value is exactly predictable - at a 7680x4320 buffer, centre screen
    // is 3840*16 = 61440, which as an int16 is -4096, and 2160*16 = 34560 is -30976 - so the
    // pair can be searched for directly. Whatever writes it next is the code that has been
    // destroying the crosshair, without needing to guess its instruction encoding.
    // Written on a timer purely so slot 0 has something to fire on.
    volatile int16_t g_WatchSelfTest = 0;

    // The render size is copied into float fields during init (mgs4.exe+4E1D00 does exactly
    // this, storing float(bufferSizeX) at [rbx+0xD4] and [rbx+0xE4]). Code working from those
    // copies never touches the global, which is why a watch on the global cannot see it.
    //
    // Those copies are findable: adjacent floats holding exactly the buffer width and height.
    int ScanForCachedRenderSize()
    {
        using namespace RenderPipeline;

        if (!pInternalResX || !pInternalResY)
        {
            return 0;
        }

        const auto wantX = static_cast<float>(*pInternalResX);
        const auto wantY = static_cast<float>(*pInternalResY);

        spdlog::info("MGS4: Cached-size scan: looking for adjacent floats ({}, {}).", wantX, wantY);

        std::vector<uintptr_t> candidates;
        uint8_t* cursor = nullptr;
        uint64_t scanned = 0;
        constexpr uint64_t kScanLimit = 4ull * 1024 * 1024 * 1024;

        while (scanned < kScanLimit && candidates.size() < 256)
        {
            MEMORY_BASIC_INFORMATION mbi {};
            if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0)
            {
                break;
            }

            auto* const region = static_cast<uint8_t*>(mbi.BaseAddress);
            const size_t size = mbi.RegionSize;

            const bool usable = mbi.State == MEM_COMMIT
                && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY)
                && (mbi.Protect & PAGE_GUARD) == 0;

            if (usable && size >= 8)
            {
                for (size_t offset = 0; offset + 8 <= size; offset += 4)
                {
                    float x {};
                    float y {};
                    std::memcpy(&x, region + offset, sizeof(x));
                    std::memcpy(&y, region + offset + 4, sizeof(y));

                    if (x == wantX && y == wantY)
                    {
                        candidates.push_back(reinterpret_cast<uintptr_t>(region + offset));
                        if (candidates.size() >= 256)
                        {
                            break;
                        }
                    }
                }
                scanned += size;
            }

            cursor = region + size;
        }

        // GPU constant and upload buffers are recycled within a frame or two, so anything
        // holding these values transiently stops matching almost immediately. The game's own
        // cached copy is written once during init and stays. Waiting and re-checking separates
        // them - the first attempt watched four driver-side buffers and learned nothing.
        spdlog::info("MGS4: Cached-size scan: {} raw match(es); re-checking after 400ms for stability.",
            candidates.size());

        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        int found = 0;
        for (const uintptr_t address : candidates)
        {
            float x {};
            float y {};
            std::memcpy(&x, reinterpret_cast<void*>(address), sizeof(x));
            std::memcpy(&y, reinterpret_cast<void*>(address + 4), sizeof(y));

            if (x != wantX || y != wantY)
            {
                continue;
            }

            spdlog::info("MGS4: Cached-size scan: stable match at 0x{:X}.", address);
            g_BreakpointSites[found] = BreakpointSite {
                0, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, 0, true, address, true };

            if (++found >= 4)
            {
                break;
            }
        }

        g_BreakpointCount = found;
        spdlog::info("MGS4: Cached-size scan: {} stable match(es) being watched.", found);
        return found;
    }

    // Guarding the whole allocation that holds the wrapped coordinate, rather than watching a
    // couple of bytes with a debug register.
    //
    // A hardware watch only covers 2-8 bytes, so it misses a rebuild that lands at a different
    // offset - which is likely, since the wrapped value is written once when the element is
    // built. PAGE_GUARD covers every byte of the region, and a guard fault reports the
    // instruction that touched it. The buffer holds static UI geometry, so ordinary frames
    // should be quiet; the interesting fault is the one that happens when the HUD is rebuilt.
    int ScanForWrappedCoordinates();

    // The write we are after happens once, before its address can be known - so it cannot be
    // caught by pointing a probe at the value. Instead: guard the game's heap *before* the
    // crosshair exists, record every write as (address, instruction) with no logging, then
    // once the crosshair has been built, find the wrapped value and look up which recorded
    // write landed on it.
    std::vector<std::pair<uintptr_t, size_t>> g_GuardedRegions;
    ULONGLONG g_GuardStarted = 0;

    // Removes every guard so the game runs normally again.
    void DisarmAllGuards()
    {
        for (const auto& [base, size] : g_GuardedRegions)
        {
            DWORD previous = 0;
            VirtualProtect(reinterpret_cast<void*>(base), size, PAGE_READWRITE, &previous);
        }
        g_GuardedRegions.clear();
    }

    // Every committed private read/write region, not just the biggest: the vertex data turned
    // out to live outside the engine's main heap, so guarding only the largest allocation
    // missed it entirely.
    // Every thread's stack, so they can be left alone. A guard page on a live stack breaks the
    // OS's stack-growth mechanism - every push faults and the process wedges immediately.
    std::vector<uintptr_t> CollectThreadStacks()
    {
        std::vector<uintptr_t> stacks;

        const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            return stacks;
        }

        THREADENTRY32 entry {};
        entry.dwSize = sizeof(entry);
        const DWORD process = GetCurrentProcessId();

        if (Thread32First(snapshot, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID != process)
                {
                    continue;
                }

                const HANDLE thread = OpenThread(
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, entry.th32ThreadID);
                if (!thread)
                {
                    continue;
                }

                if (entry.th32ThreadID != GetCurrentThreadId())
                {
                    SuspendThread(thread);
                    CONTEXT ctx {};
                    ctx.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(thread, &ctx))
                    {
                        stacks.push_back(static_cast<uintptr_t>(ctx.Rsp));
                    }
                    ResumeThread(thread);
                }

                CloseHandle(thread);
            } while (Thread32Next(snapshot, &entry));
        }

        CloseHandle(snapshot);

        // This thread's own stack too.
        volatile int here = 0;
        stacks.push_back(reinterpret_cast<uintptr_t>(&here));
        return stacks;
    }

    bool ArmGuardOnAllHeaps()
    {
        g_GuardedRegions.clear();
        const std::vector<uintptr_t> stacks = CollectThreadStacks();

        // The recording buffer must not be guarded, or the handler faults while writing its
        // own record and takes the process down.
        const auto logBase = reinterpret_cast<uintptr_t>(g_GuardLog.data());
        const uintptr_t logEnd = logBase + (g_GuardLog.size() * sizeof(GuardRecord));

        uint8_t* cursor = nullptr;
        uint64_t total = 0;

        while (true)
        {
            MEMORY_BASIC_INFORMATION mbi {};
            if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0)
            {
                break;
            }

            const auto base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const size_t size = mbi.RegionSize;
            if (size == 0)
            {
                break;
            }

            const bool overlapsLog = (base < logEnd) && ((base + size) > logBase);

            bool isStack = false;
            for (const uintptr_t rsp : stacks)
            {
                if (rsp >= base && rsp < base + size)
                {
                    isStack = true;
                    break;
                }
            }

            // Small regions are almost all stacks, TLS and CRT bookkeeping; the UI geometry
            // lives in something much larger.
            if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE
                && mbi.Protect == PAGE_READWRITE && !overlapsLog && !isStack
                && size >= 0x10000)
            {
                DWORD previous = 0;
                if (VirtualProtect(mbi.BaseAddress, size, PAGE_READWRITE | PAGE_GUARD, &previous))
                {
                    g_GuardedRegions.emplace_back(base, size);
                    total += size;
                }
            }

            cursor = static_cast<uint8_t*>(mbi.BaseAddress) + size;
        }

        spdlog::info("MGS4: Guard: armed on {} region(s), {} MB total. Recording writes.",
            g_GuardedRegions.size(), total / (1024 * 1024));
        return !g_GuardedRegions.empty();
    }

    // Reports which recorded writes landed on or near the wrapped coordinate.
    void ReportGuardWritesNear(uintptr_t address)
    {
        const size_t count = (std::min)(g_GuardLogIndex.load(), g_GuardLog.size());
        spdlog::info("MGS4: Guard: {} write(s) recorded; looking for ones near 0x{:X}.", count, address);

        int hits = 0;
        for (size_t i = 0; i < count; i++)
        {
            const int64_t delta =
                static_cast<int64_t>(g_GuardLog[i].address) - static_cast<int64_t>(address);

            if (delta > -256 && delta < 256)
            {
                const uintptr_t rva = g_GuardLog[i].rip - reinterpret_cast<uintptr_t>(mgs4e::game::Module());
                spdlog::info("MGS4: Guard: write at {:+d} bytes by {}",
                    delta, (rva > 0x241BE000)
                        ? std::format("a module other than mgs4.exe (rip 0x{:X})", g_GuardLog[i].rip)
                        : std::format("mgs4.exe+{:X}", rva));
                hits++;
            }
        }

        spdlog::info("MGS4: Guard: {} write(s) landed within 256 bytes of the wrapped value.", hits);
    }

    void ReArmGuard();
    bool ArmGuardOn(uintptr_t address);

    // Dumps the memory around a wrapped coordinate, so the structure it lives in can be read
    // rather than guessed at. Six passes of pattern matching have each assumed a layout - that
    // the coordinate sits at [obj+0x1C], that it is narrowed by a store, that it is read as a
    // word - and every one of those assumptions produced candidates that dissolved on
    // inspection. This shows what is actually there.
    void DumpWrappedContext()
    {
        using namespace RenderPipeline;

        const int matches = ScanForWrappedCoordinates();
        if (matches <= 0)
        {
            spdlog::warn("MGS4: Dump: no wrapped coordinate found - is the crosshair on screen?");
            return;
        }

        // What the coordinate would be if nothing had wrapped, for spotting it nearby.
        const int32_t unwrappedX = (pInternalResX ? *pInternalResX / 2 : 0) * 16;
        const int32_t unwrappedY = (pInternalResY ? *pInternalResY / 2 : 0) * 16;

        for (int m = 0; m < matches; m++)
        {
            const uintptr_t found = g_BreakpointSites[m].watchAddress;
            if (found == reinterpret_cast<uintptr_t>(&g_WatchSelfTest))
            {
                continue;
            }

            // If this landed in an upload buffer we guarded at creation, we already know who
            // wrote it first - which is the whole point of guarding them.
            {
                std::lock_guard lock(g_UploadGuardMutex);

                int written = 0;
                for (const auto& guard : g_UploadGuards)
                {
                    if (guard.writer != 0) { written++; }
                }
                spdlog::info("MGS4: Upload guard: {} buffer(s) guarded, {} written by the CPU, {} read fault(s) seen.",
                    g_UploadGuards.size(), written, g_UploadGuardReads.load());

                for (const auto& guard : g_UploadGuards)
                {
                    if (found >= guard.base && found < guard.base + guard.size)
                    {
                        if (guard.writer != 0)
                        {
                            spdlog::info("MGS4: ===== the wrapped value is in an upload buffer first written by mgs4.exe+{:X} (offset +{:X} of {} bytes) =====",
                                guard.writer, found - guard.base, guard.size);
                        }
                        else
                        {
                            spdlog::info("MGS4: ===== the wrapped value is in a guarded upload buffer, but nothing has written it from the CPU =====");
                        }
                        break;
                    }
                }
            }

            spdlog::info("MGS4: ===== memory around the wrapped value at 0x{:X} =====", found);
            spdlog::info("MGS4: Dump: unwrapped would be {} / {} (buffer {}x{})",
                unwrappedX, unwrappedY,
                pInternalResX ? *pInternalResX : 0, pInternalResY ? *pInternalResY : 0);

            constexpr intptr_t kBefore = 128;
            constexpr intptr_t kAfter = 128;

            for (intptr_t delta = -kBefore; delta < kAfter; delta += 16)
            {
                const uintptr_t line = found + delta;
                if (!mgs4e::mem::Readable(reinterpret_cast<void*>(line), 16))
                {
                    continue;
                }

                uint8_t bytes[16] {};
                std::memcpy(bytes, reinterpret_cast<void*>(line), sizeof(bytes));

                std::string hex;
                for (const uint8_t b : bytes)
                {
                    hex += std::format("{:02X} ", b);
                }

                // Both readings, since the whole question is which width the data is in.
                std::string words;
                for (int i = 0; i < 8; i++)
                {
                    int16_t w {};
                    std::memcpy(&w, bytes + (i * 2), sizeof(w));
                    words += std::format("{:>7}", w);
                }

                std::string dwords;
                bool interesting = false;
                for (int i = 0; i < 4; i++)
                {
                    int32_t d {};
                    std::memcpy(&d, bytes + (i * 4), sizeof(d));
                    dwords += std::format("{:>12}", d);
                    if (d == unwrappedX || d == unwrappedY)
                    {
                        interesting = true;
                    }
                }

                spdlog::info("  {:+4d} | {}| i16{} | i32{}{}",
                    static_cast<int>(delta), hex, words, dwords,
                    interesting ? "   <== UNWRAPPED VALUE HERE" : "");
            }
        }
    }

    // Finds the wrapped coordinate inside data about to be copied to the GPU, and puts a
    // hardware write watch on it. The staging buffer is rewritten every frame, so whatever
    // produces the coordinate writes this same place again shortly - and names itself.
    void TryArmStagingWatch(const uint8_t* data, UINT64 bytes)
    {
        using namespace RenderPipeline;

        if (!pInternalResX || !pInternalResY)
        {
            return;
        }

        const auto wantX = static_cast<int16_t>((*pInternalResX / 2) * 16);
        const auto wantY = static_cast<int16_t>((*pInternalResY / 2) * 16);

        // Say when we looked and found nothing, so "no watch armed" can be told apart from
        // "the hook never ran" - that ambiguity has cost several runs already.
        bool found = false;
        static std::atomic<int> searched { 0 };
        const int attempt = searched.fetch_add(1);

        for (UINT64 at = 0; at + 4 <= bytes; at += 2)
        {
            int16_t x {};
            int16_t y {};
            std::memcpy(&x, data + at, sizeof(x));
            std::memcpy(&y, data + at + 2, sizeof(y));

            if (x != wantX || y != wantY)
            {
                continue;
            }

            found = true;
            const auto address = reinterpret_cast<uintptr_t>(data + at);
            spdlog::info("MGS4: Staging: wrapped pair ({}, {}) at 0x{:X}, offset {} of a {} byte copy - watching it.",
                x, y, address, at, bytes);

            g_BreakpointCount = 1;
            g_BreakpointSites[0] = BreakpointSite {
                0, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, 0, true, address, false };
            g_BreakpointBudget[0].store(20);

            if (!g_VehHandle)
            {
                g_VehHandle = AddVectoredExceptionHandler(1, BreakpointHandler);
            }
            ArmBreakpointsOnAllThreads();
            g_StagingWatchArmed.store(true);

            // Keep re-arming: debug registers are per-thread and the writer may live on a
            // thread that did not exist when this ran.
            std::thread([]
                {
                    for (int i = 0; i < 200; i++)
                    {
                        ArmBreakpointsOnAllThreads();
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }).detach();
            return;
        }

        if (!found && attempt < 40 && (attempt % 8) == 0)
        {
            spdlog::info("MGS4: Staging: searched {} bytes for ({}, {}) - not present in this copy.",
                bytes, wantX, wantY);
        }
    }

    // The CPU-side array the UI instance data is uploaded from.
    //
    // +7A583A calls the upload helper (+7A8CD0, which Maps, memcpys and Unmaps) with a source
    // taken from a global:
    //
    //   mov rdx, [r15+0x24D35C0]        ; descriptor
    //   movzx eax, word ptr [rdx+0x12]  ; element count
    //   imul rcx, rax, 0x78             ; 120-byte records
    //   mov rax, [rdx]                  ; the source buffer
    //
    // Unlike the D3D12 upload buffers - a 14-slot ring, which is why address watches on them
    // were so awkward - this allocation is stable, so whatever writes the wrapped anchor into
    // it can be caught with a single watch.
    constexpr uintptr_t kRvaUiInstanceDescriptor = 0x24D35C0;

    // Read the buffer straight off the instruction that loads it, instead of guessing which
    // global it comes from.
    //
    // The disassembly shows `mov rdx, [r15+0x24D35C0]`, and r15 was assumed to be the module
    // base. It is not - resolving that way produced an unreadable pointer and the scan returned
    // early without logging, which looked like the scan never ran. Hooking the instruction that
    // already holds the values removes the guess entirely:
    //
    //   +7A5828  mov rax, [rdx]          ; rax = source buffer, rdx = descriptor
    //   +7A582B  mov [rsp+0x20], rax     ; <- hook here
    // There are two of these uploads, not one, and they carry different record sizes:
    //
    //   +7A57C8  mov r8, [r15+0x24D35B8]        ; descriptor, count at [r8+0x10]
    //   +7A57D4  imul rdx, rcx, 0x70            ; 112-byte records
    //   +7A57E7  mov rdx, [r8]                  ; source
    //   +7A57EA  mov [rsp+0x20], rdx            ; <- hook B
    //
    //   +7A580A  mov rdx, [r15+0x24D35C0]       ; descriptor, count at [rdx+0x12]
    //   +7A5815  imul rcx, rax, 0x78            ; 120-byte records
    //   +7A5828  mov rax, [rdx]                 ; source
    //   +7A582B  mov [rsp+0x20], rax            ; <- hook A
    //
    // Hook A alone came back with two records of +-2.0 - not the reticle - so both are probed.
    constexpr uintptr_t kRvaUiInstanceSourceLoad = 0x7A582B;
    constexpr uintptr_t kRvaUiInstanceSourceLoadB = 0x7A57EA;

    struct UiInstanceArray
    {
        std::atomic<uint8_t*> source { nullptr };
        std::atomic<uint32_t> count { 0 };
        size_t stride;
        const char* label;
    };

    UiInstanceArray g_UiArrayA { {}, {}, 0x78, "A (0x78)" };
    UiInstanceArray g_UiArrayB { {}, {}, 0x70, "B (0x70)" };

    SafetyHookMid UiInstanceSource_hook {};
    SafetyHookMid UiInstanceSourceB_hook {};

    void ScanUiSourceNow(uint8_t* source, size_t span, size_t stride, const char* label);

    void UiInstanceSourceProbe(SafetyHookContext& ctx)
    {
        const auto descriptor = reinterpret_cast<uint8_t*>(ctx.rdx);
        uint16_t count = 0;
        if (mgs4e::mem::Readable(descriptor + 0x12, sizeof(count)))
        {
            std::memcpy(&count, descriptor + 0x12, sizeof(count));
        }

        const auto source = reinterpret_cast<uint8_t*>(ctx.rax);
        g_UiArrayA.source.store(source);
        g_UiArrayA.count.store(count);

        // Examine the buffer here, not later. This runs immediately before the memcpy that
        // uploads it, so the contents are exactly what the frame will draw. Scanning after the
        // fact reads whatever the next frame has already overwritten, which is why the array
        // came back holding nothing but +-2.0 constants.

    }

    void UiInstanceSourceProbeB(SafetyHookContext& ctx)
    {
        const auto descriptor = reinterpret_cast<uint8_t*>(ctx.r8);
        uint16_t count = 0;
        if (mgs4e::mem::Readable(descriptor + 0x10, sizeof(count)))
        {
            std::memcpy(&count, descriptor + 0x10, sizeof(count));
        }
        g_UiArrayB.source.store(reinterpret_cast<uint8_t*>(ctx.rdx));
        g_UiArrayB.count.store(count);
    }

    // Look for the wrapped anchor in a buffer about to be uploaded, and watch it where it
    // lives. This allocation is reused across frames, unlike the D3D12 upload ring, so a write
    // watch here catches the code that produces the value rather than the code that copies it.
    void ScanUiSourceNow(uint8_t* source, size_t span, size_t stride, const char* label)
    {
        static std::atomic<bool> done { false };
        if (done.load() || !source || span == 0 || span > (1u << 20)
            || !mgs4e::mem::Readable(source, span))
        {
            return;
        }

        for (size_t at = 0; at + 4 <= span; at += 4)
        {
            float value {};
            std::memcpy(&value, source + at, sizeof(value));

            if (!(std::fabs(value) > 1.02f && std::fabs(value) < 1.99f))
            {
                continue;
            }

            bool expected = false;
            if (!done.compare_exchange_strong(expected, true))
            {
                return;
            }

            const auto address = reinterpret_cast<uintptr_t>(source + at);
            spdlog::info("MGS4: UI source {}: wrapped {:.5f} at record {} offset {} (0x{:X}), "
                "{} bytes total.", label, value, at / stride, at % stride, address, span);
            ArmAnchorWatch(address, "the wrapped anchor in the UI source buffer");
            return;
        }
    }

    void ScanOneUiArray(UiInstanceArray& array)
    {
        uint8_t* const source = array.source.load();
        const uint32_t count = array.count.load();

        if (!source)
        {
            spdlog::info("MGS4: UI array {}: probe has not fired.", array.label);
            return;
        }

        const size_t span = static_cast<size_t>(count) * array.stride;
        if (count == 0 || !mgs4e::mem::Readable(source, span))
        {
            spdlog::info("MGS4: UI array {}: source 0x{:X}, count {} - not readable.",
                array.label, reinterpret_cast<uintptr_t>(source), count);
            return;
        }

        spdlog::info("MGS4: UI array {}: source 0x{:X}, {} records, {} bytes.",
            array.label, reinterpret_cast<uintptr_t>(source), count, span);

        int reported = 0;
        for (size_t at = 0; at + 4 <= span && reported < 6; at += 4)
        {
            float value {};
            std::memcpy(&value, source + at, sizeof(value));

            // The wrapped anchor specifically: outside the NDC box but still a plausible
            // single-wrap magnitude. A blanket |v| > 1.02 also catches the +-2.0 constants
            // that fill these records.
            if (!(std::fabs(value) > 1.02f && std::fabs(value) < 1.99f))
            {
                continue;
            }

            reported++;
            const auto address = reinterpret_cast<uintptr_t>(source + at);
            spdlog::info("MGS4: UI array {}: {:.5f} at record {} offset {} (0x{:X})",
                array.label, value, at / array.stride, at % array.stride, address);

            if (reported == 1)
            {
                ArmAnchorWatch(address, "the wrapped anchor in a UI instance array");
            }
        }

        if (reported == 0)
        {
            spdlog::info("MGS4: UI array {}: no wrapped NDC value present.", array.label);
        }
    }

    void ScanUiInstanceArray()
    {
        ScanOneUiArray(g_UiArrayA);
        ScanOneUiArray(g_UiArrayB);
    }

    // Search the whole process for the wrapped coordinate pair, and watch it where it lives.
    //
    // Chasing the buffer proved to be the wrong end: the vertex data reaches the GPU through a
    // Map/memcpy/Unmap helper whose source arrays turned out to hold other overlays, and the
    // D3D12 upload buffers themselves rotate through a ring. The value is a better handle. At a
    // 7680x4320 buffer the reticle's coordinate is stored as int16 -4096 (x) and -30976 (y) -
    // (bufferW/2)*16 and (bufferH/2)*16 after wrapping - and whatever game structure holds that
    // pair is a stable allocation, unlike everything downstream of it.
    void ScanProcessForWrappedPair()
    {
        using namespace RenderPipeline;

        if (!pInternalResX || !pInternalResY)
        {
            return;
        }

        const auto wantX = static_cast<int16_t>((*pInternalResX / 2) * 16);
        const auto wantY = static_cast<int16_t>((*pInternalResY / 2) * 16);

        spdlog::info("MGS4: Wrapped-pair scan: looking for ({}, {}) - the wrapped centre of a "
            "{}x{} buffer.", wantX, wantY, *pInternalResX, *pInternalResY);

        SYSTEM_INFO info {};
        GetSystemInfo(&info);

        auto* cursor = static_cast<uint8_t*>(info.lpMinimumApplicationAddress);
        auto* const limit = static_cast<uint8_t*>(info.lpMaximumApplicationAddress);

        int found = 0;
        size_t scanned = 0;

        while (cursor < limit && found < 12)
        {
            MEMORY_BASIC_INFORMATION region {};
            if (VirtualQuery(cursor, &region, sizeof(region)) != sizeof(region))
            {
                break;
            }

            auto* const next = static_cast<uint8_t*>(region.BaseAddress) + region.RegionSize;

            // Private committed read/write data only. Images are code and constants, mapped
            // sections are files, and anything guarded or no-access faults on touch - reading
            // those is how an earlier scan hung the game.
            const bool usable = region.State == MEM_COMMIT
                && region.Type == MEM_PRIVATE
                && (region.Protect == PAGE_READWRITE)
                && region.RegionSize <= (64u << 20);

            if (usable)
            {
                auto* const base = static_cast<uint8_t*>(region.BaseAddress);
                scanned += region.RegionSize;

                for (size_t at = 0; at + 4 <= region.RegionSize && found < 12; at += 2)
                {
                    int16_t x {};
                    int16_t y {};
                    std::memcpy(&x, base + at, sizeof(x));
                    std::memcpy(&y, base + at + 2, sizeof(y));

                    if (x != wantX || y != wantY)
                    {
                        continue;
                    }

                    found++;
                    const auto address = reinterpret_cast<uintptr_t>(base + at);
                    spdlog::info("MGS4: Wrapped-pair scan: ({}, {}) at 0x{:X}",
                        x, y, address);

                    if (found == 1)
                    {
                        ArmAnchorWatch(address, "the wrapped coordinate pair");
                    }
                }
            }

            cursor = next;
        }

        spdlog::info("MGS4: Wrapped-pair scan: {} hit(s) across {} MB of private memory.",
            found, scanned / (1024 * 1024));
    }

    void ArmAnchorWatch(uintptr_t address, const char* what)
    {
        static std::atomic<bool> armed { false };
        bool expected = false;
        if (!armed.compare_exchange_strong(expected, true))
        {
            return;
        }

        spdlog::info("MGS4: Anchor watch: arming a write watch on {} at 0x{:X}.", what, address);

        g_BreakpointCount = 1;
        g_BreakpointSites[0] = BreakpointSite {
            0, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, 0, true, address, false, true };
        g_BreakpointBudget[0].store(4000);

        if (!g_VehHandle)
        {
            g_VehHandle = AddVectoredExceptionHandler(1, BreakpointHandler);
        }
        ArmBreakpointsOnAllThreads();

        // Debug registers are per-thread, and the writer may be on a thread that did not exist
        // when this ran, so keep re-arming for a while.
        std::thread([]
            {
                for (int i = 0; i < 200; i++)
                {
                    ArmBreakpointsOnAllThreads();
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }).detach();
    }

    void StartTrapHotkeys()
    {
        spdlog::info("MGS4: Trap: F9 arms every candidate site (weapon LOWERED), F10 disarms and reports.");

        std::thread([]
            {
                bool armWasDown = false;
                bool stopWasDown = false;
                ULONGLONG lastPress = 0;

                while (true)
                {
                    const ULONGLONG now = GetTickCount64();
                    const bool debounced = (now - lastPress) > 1500;
                    const bool armDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
                    const bool stopDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;

                    if (armDown && !armWasDown && debounced)
                    {
                        lastPress = now;

                        if (RenderPipeline::bDumpWrappedContext)
                        {
                            DumpWrappedContext();
                        }
                        else if (!g_TrapActive.load())
                        {
                            ArmTrapSites();
                        }
                    }

                    if (stopDown && !stopWasDown && debounced && g_TrapActive.load())
                    {
                        lastPress = now;
                        DisarmTrapSites();

                        std::lock_guard lock(g_TrapMutex);
                        int live = 0;
                        for (const auto& site : g_TrapSites)
                        {
                            if (site.hits > 0)
                            {
                                live++;
                                spdlog::info("MGS4: Trap: +{:X} executed {} time(s).", site.rva, site.hits);
                            }
                        }
                        spdlog::info("MGS4: Trap: {} of {} candidate site(s) executed.",
                            live, g_TrapSites.size());
                    }

                    armWasDown = armDown;
                    stopWasDown = stopDown;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }).detach();
    }

    // Separate keys for the two stages, and a debounce. A single toggle key desynchronised the
    // moment a keypress registered twice, which silently inverted the meaning of every press
    // after it.
    void StartGuardHotkeys()
    {
        spdlog::info("MGS4: Guard: F9 starts recording heap writes (do it with the weapon LOWERED), F10 stops and reports.");

        std::thread([]
            {
                bool startWasDown = false;
                bool stopWasDown = false;
                bool aimWasDown = false;
                ULONGLONG lastPress = 0;
                int ticks = 0;

                while (true)
                {
                    const ULONGLONG now = GetTickCount64();
                    const bool debounced = (now - lastPress) > 1500;

                    const bool startDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
                    const bool stopDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;

                    if (startDown && !startWasDown && debounced && !g_GuardRecording.load())
                    {
                        lastPress = now;
                        g_GuardLog.assign(400000, GuardRecord {});
                        g_GuardLogIndex.store(0);
                        g_GuardRecording.store(true);
                        g_GuardStarted = now;
                        ArmGuardOnAllHeaps();
                    }

                    if (stopDown && !stopWasDown && debounced && g_GuardRecording.load())
                    {
                        lastPress = now;
                        g_GuardRecording.store(false);
                        g_GuardedBase = 0;
                        DisarmAllGuards();

                        const int matches = ScanForWrappedCoordinates();
                        bool reported = false;

                        for (int i = 0; i < matches; i++)
                        {
                            const uintptr_t candidate = g_BreakpointSites[i].watchAddress;
                            if (candidate != reinterpret_cast<uintptr_t>(&g_WatchSelfTest))
                            {
                                ReportGuardWritesNear(candidate);
                                reported = true;
                            }
                        }

                        if (!reported)
                        {
                            spdlog::warn("MGS4: Guard: no wrapped coordinate in memory - was the crosshair on screen when you pressed F10?");
                        }
                    }

                    // Region mode: F9 locates the allocation once, then every aim press
                    // re-arms the guard. A guard is one-shot per page, so arming it at the
                    // moment aiming begins is what catches the build - pressing a key by hand
                    // is always either too early or too late.
                    if (RenderPipeline::bGuardWrappedRegion)
                    {
                        if (startDown && !startWasDown && debounced)
                        {
                            lastPress = now;
                            g_GuardBudget.store(2000);
                            {
                                std::lock_guard lock(g_GuardWritersMutex);
                                g_GuardWriters.clear();
                            }

                            const int matches = ScanForWrappedCoordinates();
                            uintptr_t target = 0;
                            for (int i = 0; i < matches; i++)
                            {
                                const uintptr_t candidate = g_BreakpointSites[i].watchAddress;
                                if (candidate != reinterpret_cast<uintptr_t>(&g_WatchSelfTest))
                                {
                                    target = candidate;
                                    break;
                                }
                            }

                            if (target != 0 && ArmGuardOn(target))
                            {
                                spdlog::info("MGS4: Guard: watching region 0x{:X}..0x{:X} ({} KB) around the wrapped value at 0x{:X}. Re-arms on every aim.",
                                    g_GuardedBase, g_GuardedBase + g_GuardedSize,
                                    g_GuardedSize / 1024, target);
                            }
                            else
                            {
                                spdlog::warn("MGS4: Guard: no wrapped value found - aim first, then press F9.");
                            }
                        }

                        const bool aimDown = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
                        if (aimDown && !aimWasDown && g_GuardedBase != 0)
                        {
                            ReArmGuard();
                        }
                        aimWasDown = aimDown;
                    }

                    startWasDown = startDown;
                    stopWasDown = stopDown;

                    // Recording is expensive enough to make the game barely playable, so it
                    // disarms itself. Guarding every heap once froze the game outright; this
                    // makes the worst case recoverable without killing the process.
                    if (g_GuardRecording.load() && (now - g_GuardStarted) > 8000)
                    {
                        g_GuardRecording.store(false);
                        DisarmAllGuards();
                        spdlog::warn("MGS4: Guard: recording auto-stopped after 8s. Press F10 to report, or F9 to record again.");
                    }

                    if (g_GuardRecording.load() && (++ticks % 6) == 0)
                    {
                        ReArmGuard();
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }).detach();
    }

    bool ArmGuardOn(uintptr_t address)
    {
        MEMORY_BASIC_INFORMATION mbi {};
        if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0
            || mbi.State != MEM_COMMIT)
        {
            return false;
        }

        // The whole containing region. Two pages caught nothing, because the write happens
        // once when the element is built and may land anywhere in the allocation - and
        // guarding every heap in the process to catch it froze the game outright, twice.
        // One region is both safe and broad enough.
        g_GuardWatchedValue = address;
        g_GuardedBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        g_GuardedSize = mbi.RegionSize;

        DWORD previous = 0;
        return VirtualProtect(reinterpret_cast<void*>(g_GuardedBase), g_GuardedSize,
            mbi.Protect | PAGE_GUARD, &previous) != 0;
    }

    // A guard fault clears the guard bit for the page, so it has to be re-applied. Doing that
    // from the handler would fault again on the same instruction forever; a timer re-arms it
    // after execution has moved on.
    void ReArmGuard()
    {
        // Recording mode re-arms every guarded region; a guard fault clears the bit for the
        // page it hit, so without this the coverage decays to nothing within a frame or two.
        if (g_GuardRecording.load())
        {
            for (const auto& [base, size] : g_GuardedRegions)
            {
                DWORD previous = 0;
                VirtualProtect(reinterpret_cast<void*>(base), size,
                    PAGE_READWRITE | PAGE_GUARD, &previous);
            }
            return;
        }

        if (g_GuardedBase == 0 || g_GuardBudget.load() <= 0)
        {
            return;
        }

        MEMORY_BASIC_INFORMATION mbi {};
        if (VirtualQuery(reinterpret_cast<void*>(g_GuardedBase), &mbi, sizeof(mbi)) == 0
            || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0)
        {
            return;
        }

        DWORD previous = 0;
        VirtualProtect(reinterpret_cast<void*>(g_GuardedBase), g_GuardedSize,
            mbi.Protect | PAGE_GUARD, &previous);
    }

    void ArmRenderSizeWatch()
    {
        using namespace RenderPipeline;

        if (!pInternalResX)
        {
            spdlog::error("MGS4: Render size watch: globals not resolved.");
            return;
        }

        // The handler normally comes from InstallBreakpointProbe, which does not run for this
        // mode. Arming debug registers without one means the exceptions go nowhere.
        if (!g_VehHandle)
        {
            g_VehHandle = AddVectoredExceptionHandler(1, BreakpointHandler);
            spdlog::info("MGS4: Render size watch: exception handler {}.",
                g_VehHandle ? "registered" : "FAILED");
        }

        // One slot, four bytes, break on read or write.
        g_BreakpointCount = 1;
        g_BreakpointSites[0] = BreakpointSite {
            0, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, 0, true,
            reinterpret_cast<uintptr_t>(pInternalResX) };
        g_BreakpointSites[0].isRenderSizeWatch = true;

        g_BreakpointBudget[0].store(100000);

        // Arming from a dedicated thread, not from here. This runs inside the resolution hook
        // on the game's main thread, and arming skips its own caller - so the thread doing
        // most of the reading would be the one thread never watched. Repeating also covers
        // the render and worker threads, which do not exist yet at this point.
        std::thread([]
            {
                bool wasDown = false;
                int ticks = 0;

                while (true)
                {
                    // F9 forgets everything seen so far. Most of the readers fire once during
                    // load; pressing this with the weapon lowered and then aiming isolates the
                    // handful that run while the crosshair is actually being built.
                    const bool isDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
                    if (isDown && !wasDown)
                    {
                        {
                            std::lock_guard lock(g_RenderSizeReadersMutex);
                            const size_t forgotten = g_RenderSizeReaders.size();
                            g_RenderSizeReaders.clear();
                            spdlog::info("MGS4: ===== RenderSize readers cleared ({} forgotten) - anything below is new =====",
                                forgotten);
                        }

                        // Re-point the watches at the cached float copies. They only exist once
                        // the renderer has initialised, so this cannot be done at startup.
                        if (RenderPipeline::bWatchCachedRenderSize)
                        {
                            ScanForCachedRenderSize();
                            for (auto& slot : g_BreakpointBudget) { slot.store(100000); }
                        }

                        if (RenderPipeline::bGuardWrappedRegion)
                        {
                            if (ScanForWrappedCoordinates() > 0)
                            {
                                {
                                    std::lock_guard lock(g_GuardWritersMutex);
                                    g_GuardWriters.clear();
                                }
                                g_GuardBudget.store(2000);

                                // Slot 0 is the self-test variable, not a real match - guard
                                // the first site that is actually in the game's memory.
                                uintptr_t target = 0;
                                for (int i = 0; i < g_BreakpointCount; i++)
                                {
                                    const uintptr_t candidate = g_BreakpointSites[i].watchAddress;
                                    if (candidate != reinterpret_cast<uintptr_t>(&g_WatchSelfTest))
                                    {
                                        target = candidate;
                                        break;
                                    }
                                }

                                const bool armed = (target != 0) && ArmGuardOn(target);
                                spdlog::info("MGS4: Guard: {} region 0x{:X}..0x{:X} containing the wrapped value at 0x{:X}.",
                                    armed ? "armed on" : "FAILED to arm",
                                    g_GuardedBase, g_GuardedBase + g_GuardedSize, target);
                            }
                            else
                            {
                                spdlog::warn("MGS4: Guard: no wrapped value found - is the crosshair on screen?");
                            }
                        }
                    }
                    wasDown = isDown;

                    if ((++ticks % 6) == 0)
                    {
                        ArmBreakpointsOnAllThreads();
                        ReArmGuard();
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }).detach();

        spdlog::info("MGS4: Render size watch: arming thread started for bufferSizeX at 0x{:X}. Logging distinct callers.",
            reinterpret_cast<uintptr_t>(pInternalResX));
    }

    int ScanForWrappedCoordinates()
    {
        using namespace RenderPipeline;

        if (!pInternalResX || !pInternalResY)
        {
            spdlog::error("MGS4: Wrapped-value scan: render size not resolved.");
            return 0;
        }

        const auto wrappedX = static_cast<int16_t>((*pInternalResX / 2) * 16);
        const auto wrappedY = static_cast<int16_t>((*pInternalResY / 2) * 16);

        spdlog::info("MGS4: Wrapped-value scan: looking for the pair ({}, {}) - centre screen at {}x{} once wrapped.",
            wrappedX, wrappedY, *pInternalResX, *pInternalResY);

        // Slot 0 is a self-test: a variable this mod writes on a timer. If it never reports,
        // the data-watch encoding is broken and the game's silence means nothing. If it does
        // report while the game's addresses stay quiet, the CPU really is not touching them -
        // which would mean the buffer is GPU-visible and read by DMA, invisible to debug
        // registers.
        g_WatchSelfTest = 0;
        g_BreakpointSites[0] = BreakpointSite {
            0, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, 0, true,
            reinterpret_cast<uintptr_t>(&g_WatchSelfTest) };

        int found = 1;
        uint8_t* cursor = nullptr;
        uint64_t scanned = 0;
        constexpr uint64_t kScanLimit = 4ull * 1024 * 1024 * 1024;

        while (scanned < kScanLimit && found < 4)
        {
            MEMORY_BASIC_INFORMATION mbi {};
            if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0)
            {
                break;
            }

            auto* const region = static_cast<uint8_t*>(mbi.BaseAddress);
            const size_t size = mbi.RegionSize;

            const bool usable = mbi.State == MEM_COMMIT
                && (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_WRITECOPY)
                && (mbi.Protect & PAGE_GUARD) == 0;

            if (usable && size >= 4)
            {
                for (size_t offset = 0; offset + 4 <= size; offset += 2)
                {
                    int16_t x {};
                    int16_t y {};
                    std::memcpy(&x, region + offset, sizeof(x));
                    std::memcpy(&y, region + offset + 2, sizeof(y));

                    if (x == wrappedX && y == wrappedY)
                    {
                        const auto address = reinterpret_cast<uintptr_t>(region + offset);
                        spdlog::info("MGS4: Wrapped-value scan: match at 0x{:X}.", address);

                        g_BreakpointSites[found] = BreakpointSite {
                            0, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, 0, true, address };

                        if (++found >= 3)
                        {
                            break;
                        }
                    }
                }
                scanned += size;
            }

            cursor = region + size;
        }

        g_BreakpointCount = found;
        spdlog::info("MGS4: Wrapped-value scan: {} match(es), watching {} of them.", found, found);
        return found;
    }

    void InstallBreakpointProbe()
    {
        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

        // Parse the configured RVA list, decoding each so the handler knows which register
        // carries the value and where it is headed.
        std::string list = RenderPipeline::sBreakpointRvas;
        size_t pos = 0;

        while (pos < list.size() && g_BreakpointCount < 4)
        {
            size_t comma = list.find(',', pos);
            if (comma == std::string::npos)
            {
                comma = list.size();
            }

            std::string item = list.substr(pos, comma - pos);
            std::erase_if(item, [](unsigned char c) { return std::isspace(c); });
            pos = comma + 1;

            if (item.empty())
            {
                continue;
            }

            uintptr_t rva = 0;
            try
            {
                rva = static_cast<uintptr_t>(std::stoull(item, nullptr, 16));
            }
            catch (const std::exception&)
            {
                spdlog::error("MGS4: Breakpoint probe: '{}' is not a hex RVA.", item);
                continue;
            }

            BreakpointSite site { rva, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, 0 };

            auto* const at = reinterpret_cast<uint8_t*>(mgs4e::game::Module()) + rva;
            if (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, at, 16, &instruction, operands))
                && instruction.operand_count_visible >= 2)
            {
                if (operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER)
                {
                    site.valueReg = operands[1].reg.value;
                }
                if (operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY)
                {
                    site.baseReg = operands[0].mem.base;
                    site.displacement = operands[0].mem.disp.value;
                }
            }

            g_BreakpointSites[g_BreakpointCount++] = site;
            spdlog::info("MGS4: Breakpoint probe: slot {} watching mgs4.exe+{:X}.",
                g_BreakpointCount - 1, rva);
        }

        if (g_BreakpointCount == 0 && !RenderPipeline::bWatchWrappedValue)
        {
            spdlog::warn("MGS4: Breakpoint probe: no valid RVAs configured.");
            return;
        }

        // The self-test writer has to be its own thread: arming skips the thread doing the
        // arming, so a variable written by that thread would never be watched, and the
        // control would report nothing whether or not the encoding works.
        std::thread([]
            {
                while (true)
                {
                    g_WatchSelfTest = static_cast<int16_t>(GetTickCount64() & 0x7FFF);
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
            }).detach();

        g_VehHandle = AddVectoredExceptionHandler(1, BreakpointHandler);
        spdlog::info("MGS4: Breakpoint probe: handler {}. Press F9 in-game to arm {} breakpoint(s) and log 100 hits.",
            g_VehHandle ? "registered" : "FAILED", g_BreakpointCount);

        std::thread([]
            {
                bool wasDown = false;
                bool armed = false;
                int ticks = 0;

                while (true)
                {
                    const bool isDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
                    if (isDown && !wasDown)
                    {
                        // In watch mode the sites are not known ahead of time - they are
                        // wherever the wrapped coordinate currently lives, which is only true
                        // while the broken element is on screen. So scan on the keypress.
                        if (RenderPipeline::bWatchWrappedValue
                            && ScanForWrappedCoordinates() == 0)
                        {
                            spdlog::warn("MGS4: Wrapped-value scan: nothing found - is the affected element on screen?");
                        }
                        else
                        {
                            for (auto& slot : g_BreakpointBudget) { slot.store(25); }
                            armed = true;
                            ArmBreakpointsOnAllThreads();
                            spdlog::info("MGS4: Breakpoint probe: armed.");
                        }
                    }
                    wasDown = isDown;

                    // Re-arm periodically so threads created after arming are covered too -
                    // but poll the key far more often than that, or a normal keypress falls
                    // between polls and never registers.
                    if (armed && (++ticks % 12) == 0)
                    {
                        ArmBreakpointsOnAllThreads();
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }).detach();
    }

#if MGS4E_LAB_BUILD
    // A mid hook takes a plain function pointer, so one distinct function per site is
    // generated from the index rather than captured.
    template <size_t Index>
    void FixedPointProbeThunk(safetyhook::Context64& ctx)
    {
        if (g_FixedPointProbeBudget.fetch_sub(1) <= 0)
        {
            return;
        }

        const uint64_t raw = ReadContextRegister(ctx, g_FixedPointStoreRegs[Index]);
        const auto stored = static_cast<int16_t>(raw & 0xFFFF);
        const auto intended = static_cast<int32_t>(raw & 0xFFFFFFFF);

        spdlog::info("MGS4: Fixed-point probe: +{:X}  stores {} ({:.1f}px); full value {} ({:.1f}px){}",
            kFixedPointStores[Index], stored, stored / 16.0,
            intended, intended / 16.0,
            (stored != intended) ? "   <== TRUNCATED" : "");
    }

    template <size_t... Indices>
    constexpr auto MakeFixedPointProbeThunks(std::index_sequence<Indices...>)
    {
        return std::array<safetyhook::MidHookFn, sizeof...(Indices)> { &FixedPointProbeThunk<Indices>... };
    }

    const auto kFixedPointProbeThunks =
        MakeFixedPointProbeThunks(std::make_index_sequence<std::size(kFixedPointStores)>{});

    void InstallFixedPointProbe()
    {
        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

        int installed = 0;
        g_FixedPointProbeHooks.reserve(std::size(kFixedPointStores));

        for (size_t i = 0; i < std::size(kFixedPointStores); i++)
        {
            auto* const at = reinterpret_cast<uint8_t*>(mgs4e::game::Module()) + kFixedPointStores[i];

            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, at, 16, &instruction, operands))
                || instruction.operand_count_visible < 2
                || operands[1].type != ZYDIS_OPERAND_TYPE_REGISTER)
            {
                spdlog::warn("MGS4: Fixed-point probe: +{:X} did not decode as a register store, skipping.",
                    kFixedPointStores[i]);
                continue;
            }

            // A mid hook overwrites the instruction with a 5-byte jump. Hooking anything
            // shorter silently eats whatever follows, which crashes the moment a branch lands
            // in the middle of it - "mov [rdx], ax" is three bytes, and hooking fifteen of
            // these without checking took the game down.
            if (instruction.length < 5)
            {
                spdlog::warn("MGS4: Fixed-point probe: +{:X} is only {} bytes, too short to hook safely. Skipping.",
                    kFixedPointStores[i], instruction.length);
                continue;
            }

            g_FixedPointStoreRegs[i] = operands[1].reg.value;

            auto hook = safetyhook::create_mid(at, kFixedPointProbeThunks[i]);

            if (hook)
            {
                g_FixedPointProbeHooks.emplace_back(std::move(hook));
                installed++;
            }
        }

        spdlog::info("MGS4: Fixed-point probe: {} of {} store hooks installed. Press F9 in-game to log 200 conversions.",
            installed, std::size(kFixedPointStores));

        std::thread([]
            {
                bool wasDown = false;
                while (true)
                {
                    const bool isDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
                    if (isDown && !wasDown)
                    {
                        g_FixedPointProbeBudget.store(200);
                        spdlog::info("MGS4: Fixed-point probe: armed.");
                    }
                    wasDown = isDown;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }).detach();
    }
#endif

    // ---- UI layout converter probe --------------------------------------------------------
    //
    // The game converts UI layout from its logical 1280x720 space into output pixels in one
    // CPU-side function, at mgs4.exe+439810 (identified by the mgs4Ultra120 project; verified
    // in-process here before hooking). Its native math is a plain linear stretch, which is why
    // the HUD distorts at any non-16:9 output.
    //
    // This probe logs the rectangles flowing through it, to answer the question every aspect
    // fix design hangs on: is the converter called once per widget, or once per sub-quad? An
    // anchored remap (corner widgets to real corners, each element undistorted) is only viable
    // per widget - per quad it would tear multi-part widgets apart, each piece anchoring to a
    // different point.
    //
    // Logging only. Nothing is modified; the original runs for every call.

    constexpr uintptr_t kUiLayoutConverterRva = 0x439810;

    SafetyHookInline UiLayoutConverter_hook {};
    std::atomic<int> g_UiLayoutLogBudget { 2000 };
    std::atomic<uint64_t> g_UiLayoutCallCount { 0 };

    uint8_t UiLayoutConverterProbe(uintptr_t object, int32_t x, int32_t y, int32_t width, int32_t height)
    {
        const uint64_t count = g_UiLayoutCallCount.fetch_add(1) + 1;
        const bool logging = g_UiLayoutLogBudget.fetch_sub(1) > 0;
        if (!logging)
        {
            g_UiLayoutLogBudget.store(0);
            return UiLayoutConverter_hook.stdcall<uint8_t>(object, x, y, width, height);
        }

        // The first run of this probe established that every call during boot, menus, stage
        // load and aiming is the full canvas (0,0 1280x720) from one caller - the converter
        // runs once per layout object at creation, and per-widget geometry never passes
        // through it. So the interesting part is not the rect, it is what gets written into
        // the object: that is the canvas mapping every widget in the object inherits.
        //
        // A dump of the object's first 0x40 bytes caught only a flag update at +0x18. The
        // payload lives higher: a physical viewport rect at +0x200 and the logical range that
        // maps onto it at +0x210 (format documented by mgs4Ultra120's reimplementation;
        // dumped here to verify rather than trust). +0x1F8 is included as a two-word margin.
        constexpr uintptr_t kDumpBase = 0x1F8;
        uint32_t before[12] {};
        std::memcpy(before, reinterpret_cast<const void*>(object + kDumpBase), sizeof(before));

        const uint8_t result = UiLayoutConverter_hook.stdcall<uint8_t>(object, x, y, width, height);

        uint32_t after[12] {};
        std::memcpy(after, reinterpret_cast<const void*>(object + kDumpBase), sizeof(after));

        const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        spdlog::info("MGS4: UI layout #{}: obj={:X} rect=({},{} {}x{}) ret={} from {}{:X}",
            count, object, x, y, width, height, result,
            (caller >= base) ? "+" : "@",
            (caller >= base) ? caller - base : caller);

        // The object's header words, raw. Hunting for its identity: either the la2 file's
        // strcode (the archive filenames are strcodes) or a pointer into the la2 buffer the
        // archive read just logged - either would map every layout object to its file.
        for (int row = 0; row < 3; row++)
        {
            uint64_t words[4] {};
            std::memcpy(words, reinterpret_cast<const void*>(object + row * 32), sizeof(words));
            spdlog::info("MGS4: UI layout #{}:   hdr+{:02X}: {:016X} {:016X} {:016X} {:016X}",
                count, row * 32, words[0], words[1], words[2], words[3]);
        }

        for (int i = 0; i < 12; i++)
        {
            if (before[i] == after[i])
            {
                continue;
            }
            spdlog::info("MGS4: UI layout #{}:   +{:03X}: {:08X} -> {:08X}  ({} as int)",
                count, kDumpBase + i * 4, before[i], after[i], static_cast<int32_t>(after[i]));
        }

        return result;
    }

    void InstallUiLayoutProbe()
    {
        auto* const at = reinterpret_cast<uint8_t*>(mgs4e::game::Module()) + kUiLayoutConverterRva;

        // Verify before trusting: the RVA comes from another project's research against one
        // specific executable build, and .text is encrypted on disk so this is the only place
        // it can be checked. Log the bytes either way - if they ever stop matching, the log
        // should say what was actually there.
        uint8_t head[16] {};
        std::memcpy(head, at, sizeof(head));
        spdlog::info("MGS4: UI layout probe: bytes at +{:X}: {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X} {:02X}",
            kUiLayoutConverterRva, head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);

        // MSVC's canonical home-register prologue, mov [rsp+8], rbx. Anything else means a
        // different build of the executable, and hooking would corrupt whatever is there.
        constexpr uint8_t kExpectedPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08 };
        if (std::memcmp(head, kExpectedPrologue, sizeof(kExpectedPrologue)) != 0)
        {
            spdlog::error("MGS4: UI layout probe: +{:X} does not start with the expected prologue. "
                "Wrong executable build - not hooking.", kUiLayoutConverterRva);
            return;
        }

        UiLayoutConverter_hook = safetyhook::create_inline(at,
            reinterpret_cast<void*>(&UiLayoutConverterProbe));
        if (!UiLayoutConverter_hook)
        {
            spdlog::error("MGS4: UI layout probe: hook failed to install.");
            return;
        }

        spdlog::info("MGS4: UI layout probe: installed at +{:X}. Logging the first 2000 calls; "
            "F6 arms another 2000.", kUiLayoutConverterRva);

        std::thread([]
            {
                bool wasDown = false;
                while (true)
                {
                    const bool isDown = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
                    if (isDown && !wasDown)
                    {
                        g_UiLayoutLogBudget.store(2000);
                        spdlog::info("MGS4: UI layout probe: re-armed at call #{}.",
                            g_UiLayoutCallCount.load());
                    }
                    wasDown = isDown;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }).detach();
    }

    // Finds an ASCII string in the image and every instruction that references it.
    //
    // Config keys are just strings, so this locates the code that reads any given setting -
    // which is the fastest way to find out why a setting appears to be ignored. Reading it
    // from the live process rather than the file matters here: .text is encrypted on disk.
    void LogStringReferences(const std::string& needle)
    {
        auto* const base = reinterpret_cast<uint8_t*>(mgs4e::game::Module());
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto* sections = IMAGE_FIRST_SECTION(nt);

        uintptr_t textStart = 0;
        size_t textSize = 0;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
        {
            if (std::memcmp(sections[i].Name, ".text", 5) == 0)
            {
                textStart = sections[i].VirtualAddress;
                textSize = sections[i].Misc.VirtualSize;
                break;
            }
        }

        // Where the string lives. Walk the image a region at a time and search within each -
        // testing readability per byte across 605MB is a VirtualQuery per byte and never
        // finishes, which hung the game before its window ever appeared.
        std::vector<uintptr_t> found;
        const size_t imageSize = nt->OptionalHeader.SizeOfImage;

        uint8_t* cursor = base;
        uint8_t* const imageEnd = base + imageSize;

        while (cursor < imageEnd && found.size() < 16)
        {
            MEMORY_BASIC_INFORMATION mbi {};
            if (VirtualQuery(cursor, &mbi, sizeof(mbi)) == 0 || mbi.RegionSize == 0)
            {
                break;
            }

            auto* regionStart = static_cast<uint8_t*>(mbi.BaseAddress);
            uint8_t* regionEnd = regionStart + mbi.RegionSize;
            if (regionEnd > imageEnd)
            {
                regionEnd = imageEnd;
            }

            const bool readable = mbi.State == MEM_COMMIT
                && (mbi.Protect & PAGE_GUARD) == 0
                && (mbi.Protect & PAGE_NOACCESS) == 0;

            if (readable && regionEnd > regionStart)
            {
                const size_t span = static_cast<size_t>(regionEnd - regionStart);
                const char first = needle[0];

                for (size_t i = 0; i + needle.size() + 1 <= span; i++)
                {
                    if (regionStart[i] != static_cast<uint8_t>(first))
                    {
                        continue;
                    }

                    if (std::memcmp(regionStart + i, needle.c_str(), needle.size()) == 0
                        && regionStart[i + needle.size()] == '\0')
                    {
                        found.push_back(static_cast<uintptr_t>((regionStart + i) - base));
                        if (found.size() >= 16)
                        {
                            break;
                        }
                    }
                }
            }

            cursor = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
        }

        spdlog::info("MGS4: String scan: '{}' found at {} location(s).", needle, found.size());
        for (const uintptr_t rva : found)
        {
            spdlog::info("MGS4: String scan:   string at mgs4.exe+{:X}", rva);
        }

        if (found.empty() || textStart == 0)
        {
            return;
        }

        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

        int refs = 0;
        size_t offset = 0;

        while (offset < textSize)
        {
            uint8_t* const at = base + textStart + offset;

            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, at, textSize - offset,
                &instruction, operands)))
            {
                offset++;
                continue;
            }

            const uintptr_t rva = textStart + offset;

            for (ZyanU8 op = 0; op < instruction.operand_count_visible; op++)
            {
                if (operands[op].type != ZYDIS_OPERAND_TYPE_MEMORY
                    || operands[op].mem.base != ZYDIS_REGISTER_RIP)
                {
                    continue;
                }

                const uintptr_t target = rva + instruction.length
                    + static_cast<uintptr_t>(operands[op].mem.disp.value);

                for (const uintptr_t stringRva : found)
                {
                    if (target == stringRva)
                    {
                        spdlog::info("MGS4: String scan:   referenced from mgs4.exe+{:X}", rva);
                        refs++;
                    }
                }
            }

            offset += instruction.length;
        }

        spdlog::info("MGS4: String scan: {} reference(s) to '{}'.", refs, needle);
    }

    // Every instruction that addresses a given struct offset.
    //
    // The UI draw list lives at +0x14FC8 in the renderer object (mgs4.exe+7A5D97 loads its
    // address before calling the draw loop), so anything that fills that list has to reference
    // the same offset. Enumerating them finds the builder without guessing at instruction
    // shapes - which is how the last several attempts went wrong.
    void LogDisplacementReferences(int64_t displacement)
    {
        auto* const base = reinterpret_cast<uint8_t*>(mgs4e::game::Module());
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        const auto* sections = IMAGE_FIRST_SECTION(nt);

        uintptr_t textStart = 0;
        size_t textSize = 0;
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
        {
            if (std::memcmp(sections[i].Name, ".text", 5) == 0)
            {
                textStart = sections[i].VirtualAddress;
                textSize = sections[i].Misc.VirtualSize;
                break;
            }
        }

        if (textStart == 0)
        {
            return;
        }

        ZydisDecoder decoder;
        ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

        ZydisFormatter formatter;
        ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);

        spdlog::info("MGS4: Displacement scan: looking for +0x{:X}...", displacement);

        int found = 0;
        size_t offset = 0;

        while (offset < textSize)
        {
            uint8_t* const at = base + textStart + offset;

            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, at, textSize - offset,
                &instruction, operands)))
            {
                offset++;
                continue;
            }

            const uintptr_t rva = textStart + offset;

            for (ZyanU8 op = 0; op < instruction.operand_count_visible; op++)
            {
                if (operands[op].type == ZYDIS_OPERAND_TYPE_MEMORY
                    && operands[op].mem.disp.value == displacement
                    && operands[op].mem.base != ZYDIS_REGISTER_RIP)
                {
                    char text[256] {};
                    ZydisFormatterFormatInstruction(&formatter, &instruction, operands,
                        instruction.operand_count_visible, text, sizeof(text),
                        reinterpret_cast<ZyanU64>(at), ZYAN_NULL);

                    spdlog::info("    +{:X}  {}", rva, text);
                    found++;
                    break;
                }
            }

            offset += instruction.length;
        }

        spdlog::info("MGS4: Displacement scan: {} reference(s) to +0x{:X}.", found, displacement);
    }

    // F8 toggles the render buffer globals between the inflated size and the window size,
    // live, long after the render targets have been allocated from them.
    //
    // This asks one question: does the UI's NDC conversion read these globals per frame, or a
    // copy taken during init? If the UI snaps to its correct size and position on the first
    // press, it reads them live and the conversion can be reached by writing the globals at
    // the right moment. If nothing moves, it is working from a cached copy and the copy is
    // what has to be found. Repointing all 31 scale reads plus all 19 float-conversion
    // candidates left the UI half-scale in the corner, so a cached copy is the standing
    // suspicion - this settles it in one launch instead of a bisection.
    void StartUiCoordinateProbeHotkey()
    {
        // The globals are resolved later in ApplyFixes than this runs, so the thread waits for
        // them rather than checking once and giving up.
        spdlog::info("MGS4: UI coordinate probe ready. Press F8 in-game to toggle the render buffer globals between buffer and window size.");

        std::thread([]
            {
                using namespace RenderPipeline;

                // Captured on the first press, not at thread start: the engine parses its
                // config and our hook inflates the buffer well after this point.
                int bufferX = 0;
                int bufferY = 0;

                bool wasDown = false;
                bool atWindowSize = false;

                while (true)
                {
                    const bool isDown = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;

                    if (isDown && !wasDown)
                    {
                        if (!pInternalResX || !pInternalResY || !pWindowResX || !pWindowResY)
                        {
                            spdlog::warn("MGS4: UI coordinate probe: render size globals not resolved yet, ignoring.");
                            wasDown = isDown;
                            std::this_thread::sleep_for(std::chrono::milliseconds(40));
                            continue;
                        }

                        if (bufferX == 0)
                        {
                            bufferX = *pInternalResX;
                            bufferY = *pInternalResY;
                        }

                        atWindowSize = !atWindowSize;

                        const int x = atWindowSize ? *pWindowResX : bufferX;
                        const int y = atWindowSize ? *pWindowResY : bufferY;

                        mgs4e::mem::Poke<int>(reinterpret_cast<uintptr_t>(pInternalResX), x);
                        mgs4e::mem::Poke<int>(reinterpret_cast<uintptr_t>(pInternalResY), y);

                        spdlog::info("MGS4: UI coordinate probe: bufferSize globals now {}x{} ({}).",
                            x, y, atWindowSize ? "window size" : "render buffer size");
                    }

                    wasDown = isDown;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }).detach();
    }

    // F9 arms one frame's worth of pipeline state logging, so the capture is of whatever is
    // on screen right now rather than of the loading screens.
    void StartStateLogHotkey()
    {
        spdlog::info("MGS4: Pipeline state logging ready. Press F9 while aiming to log one frame.");

        std::thread([]
            {
                bool wasDown = false;
                int session = 0;

                while (true)
                {
                    const bool isDown = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;

                    if (isDown && !wasDown)
                    {
                        // A frame is roughly 60 viewports + 110 scissors + 400 draws.
                        g_StateLogSeq.store(0);
                        g_StateLogBudget.store(1400);
                        spdlog::info("MGS4: ===== state log session {} armed =====", ++session);
                    }

                    wasDown = isDown;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }).detach();
    }

    void StartPixCaptureHotkey()
    {
        spdlog::info("MGS4: PIX capture ready. Press F10 in-game to capture a frame.");

        std::thread([]
            {
                int captureIndex = 0;
                bool wasDown = false;

                while (true)
                {
                    const bool isDown = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;

                    if (isDown && !wasDown && g_PixCaptureNextFrame)
                    {
                        const std::filesystem::path target =
                            mgs4e::game::Root() / std::format("mgs4_capture_{}.wpix", ++captureIndex);

                        const HRESULT result = g_PixCaptureNextFrame(target.wstring().c_str(), 1);

                        if (SUCCEEDED(result))
                        {
                            spdlog::info("MGS4: PIX capture {} written to {}.", captureIndex, target.string());
                        }
                        else
                        {
                            spdlog::error("MGS4: PIX capture {} failed (0x{:08X}).", captureIndex, static_cast<uint32_t>(result));
                        }
                    }

                    wasDown = isDown;
                    std::this_thread::sleep_for(std::chrono::milliseconds(40));
                }
            }).detach();
    }

    // ---------------- UI coordinate space redirect (experimental) ----------------
    // UI elements are authored in a virtual 1280x720 space and scaled to the render buffer:
    //     pixel = virtual * bufferSize / 1280
    // That pixel value is then held as 16-bit fixed point at 1/16px, so a centre-screen
    // element overflows once the buffer is wider than 4095.
    //
    // The buffer size cancels out of the final NDC (2*virtual/1280 - 1), so it only affects
    // that 16-bit intermediate. Pointing these particular reads at windowSize instead keeps
    // the UI geometrically identical while halving the intermediate - provided the matching
    // NDC conversion is fed from the same place, which is what this experiment tests.
    //
    // Sites found by locating reads of the render size globals that are followed by the
    // magic constant for a divide by 1280.
    // The writers: they store a rectangle in render-buffer pixels, scaled from a virtual
    // 1280x720 layout (mgs4.exe+4E1D00 is the clearest example - it builds the full-screen
    // rect from bufferSizeX/Y, then a 506/112..1172/486 sub-rect via imul + divide-by-1280).
    constexpr uintptr_t kUiScaleReadsX[] = {
        0x439834, 0x43983F, 0x4E1D82, 0x4E1DC1, 0x4E9458, 0x4F0773, 0x5A47E3, 0x5A482E,
        0x5A49A1, 0xD1D81E, 0xD1D85E, 0xE6C288, 0xE6D15D, 0x12DB87E, 0x12DEF51, 0x12DEF9C
    };
    constexpr uintptr_t kUiScaleReadsY[] = {
        0x439859, 0x439862, 0x4E1D9D, 0x4E1DF8, 0x4E9461, 0x4F075B, 0x4FC90C, 0x50428C,
        0x5084CC, 0x5A47D8, 0xD1D839, 0xD1D895, 0xE6D163, 0x12DB88B, 0x12DEFA9
    };

    // The reader: mgs4.exe+4E14E0 converts those fields back to 1/16th of a virtual pixel
    // with "field * 1280 / bufferSizeX * 16" (constants confirmed at +1664D18/1C/20 as
    // 16.0, 720.0, 1280.0). Repointing the writers alone leaves this dividing by the render
    // buffer while the fields hold window-space values, which halves everything and pulls the
    // UI into the corner - exactly what the first redirect attempt produced. Writer and
    // reader have to move together.
    constexpr uintptr_t kUiScaleDivisorsX[] = { 0x4E1518, 0x4E1589 };
    constexpr uintptr_t kUiScaleDivisorsY[] = { 0x4E154B, 0x4E15C4 };

    constexpr uintptr_t kRvaWindowSizeX = 0x1B00460;
    constexpr uintptr_t kRvaWindowSizeY = 0x1B00464;

    // Rewrites the RIP-relative displacement of a single instruction so it reads a different
    // global. The displacement is located by checking which 4-byte field actually resolves
    // to the expected source, rather than assuming an instruction length.
    bool RepointGlobalRead(uintptr_t readRva, uintptr_t fromRva, uintptr_t toRva)
    {
        auto* const base = reinterpret_cast<uint8_t*>(mgs4e::game::Module());
        uint8_t* const instruction = base + readRva;

        const auto fromAddress = reinterpret_cast<uintptr_t>(base + fromRva);
        const auto toAddress = reinterpret_cast<uintptr_t>(base + toRva);

        // RIP-relative displacements are relative to the end of the whole instruction, which
        // is not necessarily the end of the displacement field - forms like
        // "IMUL EAX, dword ptr [rip+disp32], imm32" carry an immediate afterwards. So try
        // each plausible displacement position against each plausible trailing immediate
        // size, and accept the combination that actually resolves to the expected global.
        for (int offset = 2; offset <= 6; offset++)
        {
            uint8_t* const field = instruction + offset;
            if (!mgs4e::mem::Readable(field, sizeof(int32_t)))
            {
                continue;
            }

            int32_t displacement = 0;
            std::memcpy(&displacement, field, sizeof(displacement));

            for (const int trailing : { 0, 1, 2, 4 })
            {
                const auto instructionEnd =
                    reinterpret_cast<uintptr_t>(field) + sizeof(int32_t) + trailing;

                if (instructionEnd + displacement != fromAddress)
                {
                    continue;
                }

                const int64_t updated =
                    static_cast<int64_t>(toAddress) - static_cast<int64_t>(instructionEnd);
                if (updated < INT32_MIN || updated > INT32_MAX)
                {
                    return false;
                }

                mgs4e::mem::Poke<int32_t>(reinterpret_cast<uintptr_t>(field), static_cast<int32_t>(updated));
                return true;
            }
        }

        return false;
    }

    // Candidate sites for the UI's NDC divisor: every place the render size is converted to
    // float, which is what a "1/width" scale factor would be built from. One of these (or
    // none) is the conversion that leaves the UI at half scale in the corner once its
    // positions move to window space. Bisected via config rather than guessed at.
    struct FloatSite { uintptr_t rva; bool isY; };
    constexpr FloatSite kUiNdcCandidates[] = {
        { 0x4B3A7F, false }, { 0x4B3AE3, true  },
        { 0x4E18CD, false }, { 0x4E1907, true  },
        { 0x4E1932, false }, { 0x4E1951, true  },
        { 0x53E356, false }, { 0x53E3AC, true  },
        { 0x53E3CF, false }, { 0x53E3F2, true  },
        { 0x557BCB, false }, { 0x57EAEE, false },
        { 0xF72FA5, false }, { 0xF72FB4, true  },
        { 0xF72FF8, false }, { 0xF73032, true  },
        { 0x11D1D0C, false },
        { 0x15EF6CE, false }, { 0x15EF729, true },
    };

    void RedirectUiNdcCandidates()
    {
        const int total = static_cast<int>(std::size(kUiNdcCandidates));
        const int start = (std::max)(0, RenderPipeline::iUiNdcSiteStart);
        const int count = (RenderPipeline::iUiNdcSiteCount <= 0) ? 0 : RenderPipeline::iUiNdcSiteCount;
        const int end = (std::min)(total, start + count);

        if (count == 0)
        {
            spdlog::info("MGS4: UI NDC bisect: no sites selected ({} available).", total);
            return;
        }

        int patched = 0;
        for (int i = start; i < end; i++)
        {
            const auto& site = kUiNdcCandidates[i];
            const uintptr_t from = site.isY ? kRvaRenderBufferSizeY : kRvaRenderBufferSizeX;
            const uintptr_t to = site.isY ? kRvaWindowSizeY : kRvaWindowSizeX;

            if (RepointGlobalRead(site.rva, from, to))
            {
                patched++;
            }
            else
            {
                spdlog::warn("MGS4: UI NDC bisect: site {} (+{:X}) did not match, skipped.", i, site.rva);
            }
        }

        spdlog::info("MGS4: UI NDC bisect: patched sites [{}..{}) of {} - {} succeeded.",
            start, end, total, patched);
    }

    void RedirectUiCoordinateSpace()
    {
        int patched = 0;
        int failed = 0;

        for (const uintptr_t site : kUiScaleReadsX)
        {
            RepointGlobalRead(site, kRvaRenderBufferSizeX, kRvaWindowSizeX) ? patched++ : failed++;
        }
        for (const uintptr_t site : kUiScaleReadsY)
        {
            RepointGlobalRead(site, kRvaRenderBufferSizeY, kRvaWindowSizeY) ? patched++ : failed++;
        }

        int divisors = 0;
        for (const uintptr_t site : kUiScaleDivisorsX)
        {
            RepointGlobalRead(site, kRvaRenderBufferSizeX, kRvaWindowSizeX) ? divisors++ : failed++;
        }
        for (const uintptr_t site : kUiScaleDivisorsY)
        {
            RepointGlobalRead(site, kRvaRenderBufferSizeY, kRvaWindowSizeY) ? divisors++ : failed++;
        }

        spdlog::info("MGS4: UI coordinate space: repointed {} write(s) and {} divisor read(s) to windowSize, {} failed.",
            patched, divisors, failed);
    }

    void MGS4Fixes()
    {
        using namespace RenderPipeline;

        // The neighbour rewrites every anisotropic sampler's MaxAnisotropy whenever it is
        // loaded, so our CreateSampler hook would only ever stack on top of it.
        if (iAnisotropicFiltering > 0
            && mgs4e::compat::Yields(mgs4e::keys::Graphics, mgs4e::keys::AnisotropicFiltering))
        {
            iAnisotropicFiltering = 0;
        }

        // PIX has to be loaded before anything touches D3D12. It hooks the same entry
        // points we do, so while capturing we skip our own D3D12 hooks and leave the API
        // to PIX - but the resolution and shadow overrides below still apply, since the
        // whole point of the capture is to see the bug they produce.
        // Before the engine walks the display modes (that happens after our init, at its
        // config parse), so the walk already sees the clamp.
        if (bOverrideWindowSize)
        {
            InstallDisplayModeClamp();
            InstallRendererResolution();
        }

        bool bPixActive = false;
        if (bEnablePixCapture && LoadPixGpuCapturer())
        {
            StartPixCaptureHotkey();
            bPixActive = true;

        }

        if (!bPixActive)
        {
            // Installed unconditionally so a failed texture allocation is always reported -
            // that failure mode is silent in-game and shows up as something simply missing,
            // which is very hard to diagnose from a screenshot. The per-resource logging and
            // the render size monitor stay behind the diagnostic flag.
            InstallRenderTargetLogging();

            if (bLogRenderTargetAllocations)
            {
                StartRenderSizeMonitor();
            }
        }

        GraphicsSettings::ApplyShadowAndAntiAliasing();
        AspectRatio::ApplyFixes();

#if MGS4E_LAB_BUILD
        if (bRedirectUiCoordinateSpace)
        {
            RedirectUiCoordinateSpace();
            RedirectUiNdcCandidates();
        }

        if (bUiCoordinateProbe)
        {
            StartUiCoordinateProbeHotkey();
        }

        // Both settings take a comma-separated list of hex RVAs, so one launch can answer
        // several questions instead of one each.
        const auto forEachRva = [](const std::string& list, const char* what, auto&& action)
            {
                size_t pos = 0;
                while (pos < list.size())
                {
                    size_t comma = list.find(',', pos);
                    if (comma == std::string::npos)
                    {
                        comma = list.size();
                    }

                    std::string item = list.substr(pos, comma - pos);
                    std::erase_if(item, [](unsigned char c) { return std::isspace(c); });

                    if (!item.empty())
                    {
                        try
                        {
                            action(static_cast<uintptr_t>(std::stoull(item, nullptr, 16)));
                        }
                        catch (const std::exception&)
                        {
                            spdlog::error("MGS4: {}: '{}' is not a hex RVA.", what, item);
                        }
                    }

                    pos = comma + 1;
                }
            };

        if (!sFindDisplacement.empty())
        {
            try
            {
                LogDisplacementReferences(static_cast<int64_t>(std::stoull(sFindDisplacement, nullptr, 16)));
            }
            catch (const std::exception&)
            {
                spdlog::error("MGS4: Displacement scan: '{}' is not a hex offset.", sFindDisplacement);
            }
        }

        if (!sFindString.empty())
        {
            // Comma-separated, so a control string that is known to exist can be searched in
            // the same run - "found 0" means nothing on its own if the scanner is broken.
            size_t pos = 0;
            while (pos < sFindString.size())
            {
                size_t comma = sFindString.find(',', pos);
                if (comma == std::string::npos)
                {
                    comma = sFindString.size();
                }

                std::string item = sFindString.substr(pos, comma - pos);
                while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) { item.erase(item.begin()); }
                while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) { item.pop_back(); }

                if (!item.empty())
                {
                    LogStringReferences(item);
                }
                pos = comma + 1;
            }
        }

        if (bScanNarrowingConversions)
        {
            ScanNarrowingConversions();
        }

        if (bScanFixedPointSites)
        {
            ScanFixedPointSites();
        }

        if (bFixedPointProbe)
        {
            InstallFixedPointProbe();
        }

        if (bLogUiLayout)
        {
            InstallUiLayoutProbe();
        }

        if (!sBreakpointRvas.empty() || bWatchWrappedValue)
        {
            InstallBreakpointProbe();
        }

        forEachRva(sDisassembleRva, "Disassembly", [](uintptr_t rva)
            {
                LogDisassembly(rva, static_cast<size_t>((std::max)(16, iDisassembleBytes)));
            });

        forEachRva(sDumpFloats, "Constant dump", [](uintptr_t rva)
            {
                auto* const at = reinterpret_cast<uint8_t*>(mgs4e::game::Module()) + rva;
                if (!mgs4e::mem::Readable(at, sizeof(float)))
                {
                    spdlog::error("MGS4: Constant dump: mgs4.exe+{:X} is not readable.", rva);
                    return;
                }

                float f {};
                int32_t i {};
                std::memcpy(&f, at, sizeof(f));
                std::memcpy(&i, at, sizeof(i));
                spdlog::info("MGS4: Constant mgs4.exe+{:X} = {} (float) / {} (int32) / 0x{:08X}", rva, f, i, static_cast<uint32_t>(i));
            });
#endif

        // Before the early return: the truncation fix is about the reticle, not the render
        // size, and it costs nothing at 1.0 scale.
        GraphicsSettings::PatchReticleTruncation();

        if (fInternalResolutionScale <= 1.0 && !bOverrideWindowSize)
        {
            // Everything below this point overrides the render size, which a 1.0 scale does not
            // need. The aim watch is not part of that - it only decides when the UI diagnostics
            // sample - and returning without starting it meant g_AimHeld stayed false forever at
            // 1.0, so no UI draw was ever captured at native resolution.
            //
            // That silently made 100% untestable, which matters because 100% is the control case
            // every "does this only happen when scaled?" comparison depends on.
            if (bWatchStagingWrites)
            {
                StartAimWatchThread();

                // Records the CPU-side array the UI instance data is uploaded from, so the
                // wrapped anchor can be found in a stable allocation rather than in the
                // per-frame D3D12 ring the vertex buffers rotate through.
                UiInstanceSource_hook = safetyhook::create_mid(
                    reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module())
                        + kRvaUiInstanceSourceLoad),
                    UiInstanceSourceProbe);
                UiInstanceSourceB_hook = safetyhook::create_mid(
                    reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module())
                        + kRvaUiInstanceSourceLoadB),
                    UiInstanceSourceProbeB);
                spdlog::info("MGS4: UI instance source probes: A at +{:X} {}, B at +{:X} {}.",
                    kRvaUiInstanceSourceLoad,
                    UiInstanceSource_hook ? "installed" : "FAILED",
                    kRvaUiInstanceSourceLoadB,
                    UiInstanceSourceB_hook ? "installed" : "FAILED");
            }
            return;
        }

        // During engine init the game parses its render config keys into globals, in a
        // run of four identical "CALL strtol; MOV [global], EAX" pairs:
        //   +0x05  MOV [render.windowSizeX], EAX   (window / swapchain size)
        //   +0x10  MOV [render.windowSizeY], EAX
        //   +0x1B  MOV [render.bufferSizeX], EAX   (internal render buffer size)
        //   +0x26  MOV [render.bufferSizeY], EAX
        // bufferSize is what the engine's DynamicResolution system takes as its maximum
        // resolution, and what the render targets are sized from; windowSize drives the
        // actual window and swapchain. Overriding only bufferSize renders internally at a
        // higher resolution than the display, which the engine then scales down on output.
        //
        // Hook after the *last* of the four stores, otherwise the game's own writes land
        // on top of ours.
        // This signature is only CALL/MOV/CALL/MOV/CALL, which is far too generic to
        // trust on its own - it matches ten places in .text, five of them inside this
        // very run (the parse sequence repeats every 11 bytes, so the pattern overlaps
        // itself). Taking the first match happens to be correct today, but a game update
        // that introduces an earlier match would have us resolve displacements from
        // unrelated instructions and write the render resolution into arbitrary globals.
        //
        // So resolve render.bufferSizeX from the config key string first, then accept only
        // the match whose third store actually writes that global.
        uint8_t* RenderConfigResult = nullptr;
        {
            const auto imageStart = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
            int* const bufferSizeX = ResolveRenderConfigGlobal("render.bufferSizeX");
            int* const windowSizeX = ResolveRenderConfigGlobal("render.windowSizeX");

            if (!bufferSizeX || !windowSizeX)
            {
                spdlog::error("MGS4: Internal Resolution: could not resolve the render config globals, skipping.");
            }
            else
            {
                for (uint8_t* candidate : mgs4e::mem::FindAll(mgs4e::game::Module(),
                    "E8 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? E8 ?? ?? ?? ?? 89 05 ?? ?? ?? ?? E8 ?? ?? ?? ??"))
                {
                    auto* w = reinterpret_cast<int*>(mgs4e::mem::RipTarget(reinterpret_cast<uintptr_t>(candidate) + 0x07));
                    auto* h = reinterpret_cast<int*>(mgs4e::mem::RipTarget(reinterpret_cast<uintptr_t>(candidate) + 0x12));
                    auto* x = reinterpret_cast<int*>(mgs4e::mem::RipTarget(reinterpret_cast<uintptr_t>(candidate) + 0x1D));
                    auto* y = reinterpret_cast<int*>(mgs4e::mem::RipTarget(reinterpret_cast<uintptr_t>(candidate) + 0x28));

                    // All four stores must be the globals we expect, each pair adjacent:
                    // windowSizeX/Y first, then bufferSizeX/Y.
                    if (w != windowSizeX || h != w + 1
                        || x != bufferSizeX || y != x + 1
                        || !mgs4e::mem::Writable(w, sizeof(int) * 2)
                        || !mgs4e::mem::Writable(x, sizeof(int) * 2))
                    {
                        continue;
                    }

                    RenderConfigResult = candidate;
                    pWindowResX = w;
                    pWindowResY = h;
                    pInternalResX = x;
                    pInternalResY = y;
                    break;
                }

                if (!RenderConfigResult)
                {
                    spdlog::error("MGS4: Internal Resolution: render.bufferSizeX is at mgs4.exe+{:X}, but no signature match writes it. Skipping.",
                        reinterpret_cast<uintptr_t>(bufferSizeX) - imageStart);
                }
                else if (mgs4e::log::Verbose())
                {
                    spdlog::info("MGS4: Internal Resolution: matched at mgs4.exe+{:X}, windowSize at +{:X}/+{:X}, bufferSize at +{:X}/+{:X}.",
                        reinterpret_cast<uintptr_t>(RenderConfigResult) - imageStart,
                        reinterpret_cast<uintptr_t>(pWindowResX) - imageStart,
                        reinterpret_cast<uintptr_t>(pWindowResY) - imageStart,
                        reinterpret_cast<uintptr_t>(pInternalResX) - imageStart,
                        reinterpret_cast<uintptr_t>(pInternalResY) - imageStart);
                }
            }
        }

        if (RenderConfigResult)
        {
            static SafetyHookMid InternalResolutionMidHook {};
            InternalResolutionMidHook = safetyhook::create_mid(RenderConfigResult + 0x2C,
                [](SafetyHookContext& ctx)
                {
                    // The window override lands first, and becomes the basis the internal
                    // resolution scales from, so the two settings compose predictably:
                    // a 1920x1080 window at 200% renders internally at 3840x2160.
                    if (bOverrideWindowSize)
                    {
                        // The override must never exceed the surface it is drawn into: the
                        // engine sizes its colour targets from the window size and its depth
                        // buffers from the swap chain, and D3D11 drops every draw whose two
                        // targets disagree in size (a black screen; D3D12 draws them
                        // misaligned instead). The chain is the window's client area in
                        // windowed mode and the desktop in fullscreen, so when the user's
                        // in-game resolution or desktop is smaller than the override - the
                        // report was windowed mode with 1920x1080 chosen in-game under a
                        // larger override, 2026-09-07 - the override is shrunk to fit,
                        // keeping its aspect; the fit then places it as usual. This routine
                        // runs again on an in-game resolution change, so the cap follows it.
                        int sizeX = iWindowSizeX;
                        int sizeY = iWindowSizeY;
                        int capX = 0, capY = 0;
                        const char* capName = SurfaceSize(capX, capY);
                        if (capName && capX > 0 && capY > 0 && (sizeX > capX || sizeY > capY))
                        {
                            const double scale = (std::min)(static_cast<double>(capX) / sizeX, static_cast<double>(capY) / sizeY);
                            sizeX = static_cast<int>(std::lround(sizeX * scale)) & ~1;
                            sizeY = static_cast<int>(std::lround(sizeY * scale)) & ~1;
                            spdlog::info("MGS4: Window Size: the {}x{} override is larger than the {}x{} {}; using {}x{} (same shape) so the picture fits.",
                                iWindowSizeX, iWindowSizeY, capX, capY, capName, sizeX, sizeY);
                        }

                        spdlog::info("MGS4: Window Size: {}x{} -> {}x{}.",
                            *pWindowResX, *pWindowResY, sizeX, sizeY);

                        *pWindowResX = sizeX;
                        *pWindowResY = sizeY;
                        *pInternalResX = sizeX;
                        *pInternalResY = sizeY;
                    }

                    if (fInternalResolutionScale <= 1.0)
                    {
                        return;
                    }

                    const int baseX = *pInternalResX;
                    const int baseY = *pInternalResY;
                    if (baseX <= 0 || baseY <= 0)
                    {
                        spdlog::warn("MGS4: Internal Resolution: game reported a render buffer of {}x{}, leaving it alone.", baseX, baseY);
                        return;
                    }

                    int targetX = static_cast<int>(std::lround(baseX * fInternalResolutionScale));
                    int targetY = static_cast<int>(std::lround(baseY * fInternalResolutionScale));

                    // Keep the dimensions a multiple of 4 - the engine derives half and
                    // quarter resolution buffers from these.
                    targetX &= ~3;
                    targetY &= ~3;

                    // Cap the render buffer at the width the engine's UI vertex format can
                    // address. The ceiling is now a deliberate 8K limit rather than a bug
                    // boundary: the 16-bit narrowing that used to lose the reticle past 4095 is
                    // widened at its source, so what remains is a judgement about cost. See the
                    // note on iMaxInternalBufferWidth.
                    //
                    // Both axes are scaled together so the aspect ratio is preserved.
                    const int kMaxInternalBufferWidth = iMaxInternalBufferWidth;

                    if (targetX > kMaxInternalBufferWidth)
                    {
                        const double limit = static_cast<double>(kMaxInternalBufferWidth) / targetX;

                        spdlog::warn("MGS4: Internal Resolution: {}x{} exceeds the {}px ceiling, "
                            "clamping. Both axes scale together, so the aspect ratio is kept.",
                            targetX, targetY, kMaxInternalBufferWidth);

                        targetX = kMaxInternalBufferWidth;
                        targetY = static_cast<int>(std::lround(targetY * limit));
                        targetX &= ~3;
                        targetY &= ~3;
                    }

                    if (targetX <= baseX || targetY <= baseY)
                    {
                        spdlog::info("MGS4: Internal Resolution: nothing to do at this output size - the render buffer is already at or above the {}px limit, so the scale has no headroom. Leaving it at {}x{}.",
                            kMaxInternalBufferWidth, baseX, baseY);
                        return;
                    }

                    spdlog::info("MGS4: Internal Resolution: render buffer {}x{} -> {}x{} ({:.2f}x scale, {:.2f}x pixels). Output stays at {}x{}.",
                        baseX, baseY, targetX, targetY, fInternalResolutionScale,
                        (static_cast<double>(targetX) * targetY) / (static_cast<double>(baseX) * baseY),
                        *pWindowResX, *pWindowResY);

                    *pInternalResX = targetX;
                    *pInternalResY = targetY;
                });
            MGS4E_LOG_HOOK(InternalResolutionMidHook, "MGS4: Internal Resolution")

            // Armed here rather than on a hotkey: the UI geometry is built during load, well
            // before anything could be pressed, so the consumer we are looking for has already
            // run by the time the game is playable.
            if (bWatchRenderSizeReads)
            {
                ArmRenderSizeWatch();
            }

            // Independent of the render size watch, so the heap guard can run without also
            // paying for a debug-register watch that traps on every render size access.
            if (bWatchStagingWrites)
            {
                StartAimWatchThread();

                // Records the CPU-side array the UI instance data is uploaded from, so the
                // wrapped anchor can be found in a stable allocation rather than in the
                // per-frame D3D12 ring the vertex buffers rotate through.
                UiInstanceSource_hook = safetyhook::create_mid(
                    reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module())
                        + kRvaUiInstanceSourceLoad),
                    UiInstanceSourceProbe);
                UiInstanceSourceB_hook = safetyhook::create_mid(
                    reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(mgs4e::game::Module())
                        + kRvaUiInstanceSourceLoadB),
                    UiInstanceSourceProbeB);
                spdlog::info("MGS4: UI instance source probes: A at +{:X} {}, B at +{:X} {}.",
                    kRvaUiInstanceSourceLoad,
                    UiInstanceSource_hook ? "installed" : "FAILED",
                    kRvaUiInstanceSourceLoadB,
                    UiInstanceSourceB_hook ? "installed" : "FAILED");
            }

            if (bTrapFixedPointSites || bDumpWrappedContext)
            {
                if (!g_VehHandle)
                {
                    g_VehHandle = AddVectoredExceptionHandler(1, BreakpointHandler);
                }

                // The candidate list comes from the fixed-point scan, so run it if it has not
                // been run already.
                if (g_FixedPointCandidates.empty())
                {
                    ScanFixedPointSites();
                }
                StartTrapHotkeys();
            }

            if (bGuardRecordHeapWrites || bGuardWrappedRegion)
            {
                if (!g_VehHandle)
                {
                    g_VehHandle = AddVectoredExceptionHandler(1, BreakpointHandler);
                }
                StartGuardHotkeys();
            }
        }
    }


}


// Up to two hardware write watches on live anchor slots in the UI instance pool, so the code
// that produces each kind of anchor names itself in the log ("WATCH ... by mgs4.exe+...").
// The same debug-register machinery as the wrap-era anchor watch, but two slots: the whole
// point is comparing the static-widget producer against the dynamic-element producer.
#if MGS4E_LAB_BUILD
void RenderPipeline::ArmProducerWatch(uintptr_t address, const char* what)
{
    static std::atomic<int> nextSlot { 0 };
    const int slot = nextSlot.fetch_add(1);
    if (slot >= 2)
    {
        return;
    }

    spdlog::info("MGS4: Producer watch {}: arming on {} at 0x{:X}.", slot, what, address);

    g_BreakpointSites[slot] = BreakpointSite {
        0, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, 0, true, address, false, true };
    g_BreakpointBudget[slot].store(200);
    if (g_BreakpointCount < slot + 1)
    {
        g_BreakpointCount = slot + 1;
    }

    if (!g_VehHandle)
    {
        g_VehHandle = AddVectoredExceptionHandler(1, BreakpointHandler);
    }
    ArmBreakpointsOnAllThreads();

    // Debug registers are per-thread, and the producer may run on a thread that did not
    // exist when this armed, so keep re-arming for a while.
    std::thread([]
        {
            for (int i = 0; i < 200; i++)
            {
                ArmBreakpointsOnAllThreads();
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }).detach();
}
#endif

void RenderPipeline::ApplyFixes()
{
#ifdef BEFORE_COMPARISON_PICS
    spdlog::info("BEFORE_COMPARISON_PICS TRUE. SKIPPING RESOLUTION SCALING FIXES");
    return;
#endif

    MGS4Fixes();
}
