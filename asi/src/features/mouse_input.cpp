#include "pch.hpp"
#include "mouse_input.hpp"

#include "game.hpp"
#include "sites.hpp"

#include <atomic>
#include <climits>
#include <cstring>
#include <mutex>
#include <vector>

// NEXTRAWINPUTBLOCK names QWORD, which the Windows headers leave undefined.
typedef unsigned __int64 QWORD;

namespace MouseInput
{
    namespace
    {
        // GLFW 3.3 as built into the game. Every pattern is unique in BGFX 0.0.3 and 0.0.8.
        constexpr mgs4e::sites::Signature kPollEvents { "glfw: glfwPollEvents", 0x521290,
            "40 53 48 81 EC A0 00 00 00 45 33 C9 C7 44 24 20 01 00 00 00 45 33 C0 48 8D 4C 24 50 33 D2 FF 15 ?? ?? ?? ?? 85 C0 74 5F 83 7C 24 58 12 75 23", 0 };
        constexpr mgs4e::sites::Signature kInputCursorPos { "glfw: _glfwInputCursorPos", 0x51F710,
            "66 0F 2E 89 E8 01 00 00 75 0A 66 0F 2E 91 F0 01 00 00 74 1F 48 8B 81 20 03 00 00 F2 0F 11 89 E8 01 00 00 F2 0F 11 91 F0 01 00 00 48 85 C0", 0 };
        constexpr mgs4e::sites::Signature kInputScroll { "glfw: _glfwInputScroll", 0x51F8A0,
            "48 8B 81 30 03 00 00 48 85 C0 74 03 48 FF E0 C3 48 89 5C 24 08 48 89 7C 24 10 44 8B 15 ?? ?? ?? ?? 45 33 C9 48 8B F9", 0 };
        constexpr mgs4e::sites::Signature kGetKeyMods { "glfw: getKeyMods", 0x522800,
            "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 B9 10 00 00 00 FF 15 ?? ?? ?? ?? 0F B7 D8 B9 11 00 00 00 C1 EB 0F FF 15 ?? ?? ?? ??", 0 };
        // The button case of the window procedure: call getKeyMods; ...; call _glfwInputMouseClick.
        constexpr mgs4e::sites::Signature kClickCall { "glfw: mouse click call", 0x5234DA,
            "E8 ?? ?? ?? ?? 44 8B C8 44 8B C3 41 8B D4 48 8B CE E8 ?? ?? ?? ?? 49 8B C6 80 BC 30 80 00 00 00 01", 17 };
        // Inside glfwPollEvents: mov rbx, [rip+x] loads of _glfw.windowListHead and
        // _glfw.win32.disabledCursorWindow.
        constexpr size_t kWindowListLoad = 0x2F;
        constexpr size_t kDisabledWindowLoad = 0x187;

        // _GLFWwindow fields.
        constexpr size_t kVirtualCursorX = 0x1E8;   // double, then Y at +0x1F0
        constexpr size_t kWindowHandle = 0x360;     // win32.handle

        using CursorPosFn = void (*)(void*, double, double);
        using ScrollFn = void (*)(void*, double, double);
        using ClickFn = void (*)(void*, int, int, int);
        using ModsFn = int (*)();
        CursorPosFn g_CursorPos = nullptr;
        ScrollFn g_Scroll = nullptr;
        ClickFn g_Click = nullptr;
        ModsFn g_Mods = nullptr;
        void** g_WindowList = nullptr;
        void** g_DisabledWindow = nullptr;

        SafetyHookMid g_PollHook {};
        SafetyHookInline g_RegisterHook {};

        // Filled by the reader thread, taken once a frame by the game thread.
        struct ButtonEvent { int button; int action; };
        std::mutex g_Mutex;
        int64_t g_Dx = 0, g_Dy = 0;
        int g_WheelV = 0, g_WheelH = 0;
        ButtonEvent g_Buttons[64] {};
        int g_ButtonCount = 0;

        std::atomic<HWND> g_GameWindow { nullptr };
        std::atomic<int> g_CursorX { INT_MIN }, g_CursorY { INT_MIN };   // client position, cursor shown
        std::atomic<DWORD> g_ReaderTid { 0 };
        std::atomic<uint64_t> g_Reports { 0 }, g_Frames { 0 }, g_FedFrames { 0 }, g_DroppedRegistrations { 0 };

        uintptr_t RipTarget(const uint8_t* insn, size_t length)
        {
            int32_t disp = 0;
            std::memcpy(&disp, insn + length - 4, sizeof(disp));
            return reinterpret_cast<uintptr_t>(insn) + length + disp;
        }

        // Keeps every Raw Input registration of the mouse but ours from replacing ours (a
        // process has one registration per device type: the last one wins).
        BOOL WINAPI Hooked_RegisterRawInputDevices(PCRAWINPUTDEVICE devices, UINT count, UINT size)
        {
            if (GetCurrentThreadId() == g_ReaderTid.load() || size != sizeof(RAWINPUTDEVICE) || !devices)
            {
                return g_RegisterHook.stdcall<BOOL>(devices, count, size);
            }
            std::vector<RAWINPUTDEVICE> keep;
            for (UINT i = 0; i < count; i++)
            {
                if (devices[i].usUsagePage == 1 && devices[i].usUsage == 2)
                {
                    if (g_DroppedRegistrations.fetch_add(1) < 8)
                    {
                        spdlog::info("Mouse input: a Raw Input registration of the mouse (flags {:#x}) was kept from replacing ours.", devices[i].dwFlags);
                    }
                    continue;
                }
                keep.push_back(devices[i]);
            }
            if (keep.empty()) { return TRUE; }
            return g_RegisterHook.stdcall<BOOL>(keep.data(), static_cast<UINT>(keep.size()), size);
        }

        void Drain(bool foreground)
        {
            alignas(8) static uint8_t buffer[64 * 1024];
            int64_t dx = 0, dy = 0;
            int wheelV = 0, wheelH = 0;
            ButtonEvent events[64];
            int eventCount = 0;
            uint64_t reports = 0;
            static constexpr struct { USHORT down, up; int button; } kButtons[] {
                { RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, 0 },
                { RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, 1 },
                { RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, 2 },
                { RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_4_UP, 3 },
                { RI_MOUSE_BUTTON_5_DOWN, RI_MOUSE_BUTTON_5_UP, 4 },
            };
            for (;;)
            {
                UINT size = sizeof(buffer);
                const UINT count = GetRawInputBuffer(reinterpret_cast<RAWINPUT*>(buffer), &size, sizeof(RAWINPUTHEADER));
                if (count == 0 || count == static_cast<UINT>(-1)) { break; }
                const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buffer);
                for (UINT i = 0; i < count; i++)
                {
                    if (ri->header.dwType == RIM_TYPEMOUSE)
                    {
                        const RAWMOUSE& m = ri->data.mouse;
                        reports++;
                        if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
                        {
                            dx += m.lLastX;
                            dy += m.lLastY;
                        }
                        for (const auto& b : kButtons)
                        {
                            if ((m.usButtonFlags & b.down) && eventCount < 64) { events[eventCount++] = { b.button, 1 }; }
                            if ((m.usButtonFlags & b.up) && eventCount < 64) { events[eventCount++] = { b.button, 0 }; }
                        }
                        if (m.usButtonFlags & RI_MOUSE_WHEEL) { wheelV += static_cast<SHORT>(m.usButtonData); }
                        if (m.usButtonFlags & RI_MOUSE_HWHEEL) { wheelH += static_cast<SHORT>(m.usButtonData); }
                    }
                    ri = NEXTRAWINPUTBLOCK(ri);
                }
            }
            if (reports == 0) { return; }
            g_Reports.fetch_add(reports, std::memory_order_relaxed);
            if (!foreground) { return; }   // input made in another window is not the game's

            std::lock_guard lock(g_Mutex);
            g_Dx += dx;
            g_Dy += dy;
            g_WheelV += wheelV;
            g_WheelH += wheelH;
            for (int i = 0; i < eventCount && g_ButtonCount < 64; i++) { g_Buttons[g_ButtonCount++] = events[i]; }
        }

        LRESULT CALLBACK ReaderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
        {
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        DWORD WINAPI ReaderThread(void*)
        {
            g_ReaderTid.store(GetCurrentThreadId());
            SetThreadDescription(GetCurrentThread(), L"MGS4Enabler mouse input");
            WNDCLASSEXW wc { sizeof(wc) };
            wc.lpfnWndProc = ReaderWndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"MGS4EnablerMouseInput";
            RegisterClassExW(&wc);
            const HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
            RAWINPUTDEVICE rid { 1, 2, RIDEV_INPUTSINK | RIDEV_NOLEGACY, hwnd };
            if (!hwnd || !g_RegisterHook.stdcall<BOOL>(&rid, 1u, static_cast<UINT>(sizeof(rid))))
            {
                spdlog::error("Mouse input: Raw Input registration failed (window {}, error {}); the game keeps its own mouse path.",
                    hwnd ? "ok" : "failed", GetLastError());
                return 0;
            }
            spdlog::info("Mouse input: reading the mouse on its own thread (Raw Input, no ordinary mouse messages to the game).");

            const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            ULONGLONG lastReport = GetTickCount64();
            uint64_t lastReports = 0, lastFrames = 0, lastFed = 0;
            MSG msg;
            for (;;)
            {
                MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                const HWND game = g_GameWindow.load(std::memory_order_relaxed);
                const bool foreground = game && GetForegroundWindow() == game;
                const uint64_t before = g_Reports.load(std::memory_order_relaxed);
                Drain(foreground);
                if (foreground && g_Reports.load(std::memory_order_relaxed) != before)
                {
                    POINT p {};
                    if (GetCursorPos(&p) && ScreenToClient(game, &p))
                    {
                        g_CursorX.store(p.x, std::memory_order_relaxed);
                        g_CursorY.store(p.y, std::memory_order_relaxed);
                    }
                }
                while (PeekMessageW(&msg, nullptr, 0, WM_INPUT - 1, PM_REMOVE) || PeekMessageW(&msg, nullptr, WM_INPUT + 1, 0xFFFFFFFF, PM_REMOVE))
                {
                    DispatchMessageW(&msg);
                }

                const ULONGLONG now = GetTickCount64();
                if (now - lastReport >= 10000)
                {
                    const uint64_t r = g_Reports.load(), f = g_Frames.load(), fed = g_FedFrames.load();
                    spdlog::info("Mouse input: {:.0f} reports/s, {:.1f} frames/s, mouse handed to the game in {:.1f} frames/s.",
                        static_cast<double>(r - lastReports) * 1000.0 / static_cast<double>(now - lastReport),
                        static_cast<double>(f - lastFrames) * 1000.0 / static_cast<double>(now - lastReport),
                        static_cast<double>(fed - lastFed) * 1000.0 / static_cast<double>(now - lastReport));
                    lastReports = r; lastFrames = f; lastFed = fed; lastReport = now;
                }

                if (timer)
                {
                    LARGE_INTEGER due;
                    due.QuadPart = -10000;   // hold off 1 ms: one batch a millisecond at most
                    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                    WaitForSingleObject(timer, 5);
                }
                else
                {
                    Sleep(1);
                }
            }
        }

        // Game thread, at the start of glfwPollEvents.
        int g_LastX = INT_MIN, g_LastY = INT_MIN;
        void Feed()
        {
            g_Frames.fetch_add(1, std::memory_order_relaxed);
            void* const disabled = *g_DisabledWindow;
            void* const window = disabled ? disabled : *g_WindowList;
            if (!window) { return; }
            auto* const w = static_cast<uint8_t*>(window);
            g_GameWindow.store(*reinterpret_cast<HWND*>(w + kWindowHandle), std::memory_order_relaxed);

            int64_t dx, dy;
            int wheelV, wheelH, buttonCount;
            ButtonEvent buttons[64];
            {
                std::lock_guard lock(g_Mutex);
                dx = g_Dx; dy = g_Dy; wheelV = g_WheelV; wheelH = g_WheelH; buttonCount = g_ButtonCount;
                std::memcpy(buttons, g_Buttons, sizeof(ButtonEvent) * buttonCount);
                g_Dx = g_Dy = 0; g_WheelV = g_WheelH = 0; g_ButtonCount = 0;
            }

            bool fed = false;
            if (disabled)
            {
                // Camera mode: GLFW adds each move's delta to its virtual cursor; one sum a frame.
                if (dx || dy)
                {
                    const double* virtualCursor = reinterpret_cast<const double*>(w + kVirtualCursorX);
                    g_CursorPos(window, virtualCursor[0] + static_cast<double>(dx), virtualCursor[1] + static_cast<double>(dy));
                    fed = true;
                }
                g_LastX = g_LastY = INT_MIN;
            }
            else
            {
                const int x = g_CursorX.load(std::memory_order_relaxed), y = g_CursorY.load(std::memory_order_relaxed);
                if (x != INT_MIN && (x != g_LastX || y != g_LastY))
                {
                    g_CursorPos(window, static_cast<double>(x), static_cast<double>(y));
                    g_LastX = x; g_LastY = y;
                    fed = true;
                }
            }
            if (buttonCount)
            {
                const int mods = g_Mods();
                for (int i = 0; i < buttonCount; i++) { g_Click(window, buttons[i].button, buttons[i].action, mods); }
                fed = true;
            }
            // As GLFW's WM_MOUSEWHEEL / WM_MOUSEHWHEEL cases.
            if (wheelV) { g_Scroll(window, 0.0, static_cast<double>(wheelV) / WHEEL_DELTA); fed = true; }
            if (wheelH) { g_Scroll(window, -static_cast<double>(wheelH) / WHEEL_DELTA, 0.0); fed = true; }
            if (fed) { g_FedFrames.fetch_add(1, std::memory_order_relaxed); }
        }
    }

    void Install()
    {
        if (!bEnabled) { return; }
        uint8_t* const poll = mgs4e::sites::Address(kPollEvents);
        uint8_t* const cursorPos = mgs4e::sites::Address(kInputCursorPos);
        uint8_t* const scroll = mgs4e::sites::Address(kInputScroll);
        uint8_t* const mods = mgs4e::sites::Address(kGetKeyMods);
        uint8_t* const clickCall = mgs4e::sites::Address(kClickCall);
        if (!poll || !cursorPos || !scroll || !mods || !clickCall)
        {
            spdlog::warn("Mouse input: a GLFW site was not found; the game keeps its own mouse path.");
            return;
        }
        const uint8_t* listLoad = poll + kWindowListLoad;
        const uint8_t* disabledLoad = poll + kDisabledWindowLoad;
        if (!(listLoad[0] == 0x48 && listLoad[1] == 0x8B && listLoad[2] == 0x1D)
            || !(disabledLoad[0] == 0x48 && disabledLoad[1] == 0x8B && disabledLoad[2] == 0x1D) || clickCall[0] != 0xE8)
        {
            spdlog::warn("Mouse input: glfwPollEvents does not look as expected; the game keeps its own mouse path.");
            return;
        }
        g_WindowList = reinterpret_cast<void**>(RipTarget(listLoad, 7));
        g_DisabledWindow = reinterpret_cast<void**>(RipTarget(disabledLoad, 7));
        g_CursorPos = reinterpret_cast<CursorPosFn>(cursorPos);
        g_Scroll = reinterpret_cast<ScrollFn>(scroll);
        g_Mods = reinterpret_cast<ModsFn>(mods);
        g_Click = reinterpret_cast<ClickFn>(RipTarget(clickCall, 5));

        void* const registerFn = GetProcAddress(GetModuleHandleW(L"user32.dll"), "RegisterRawInputDevices");
        if (!registerFn)
        {
            spdlog::warn("Mouse input: RegisterRawInputDevices not found; the game keeps its own mouse path.");
            return;
        }
        g_RegisterHook = safetyhook::create_inline(registerFn, reinterpret_cast<void*>(Hooked_RegisterRawInputDevices));
        g_PollHook = safetyhook::create_mid(poll, [](SafetyHookContext&) { Feed(); });
        if (!g_RegisterHook || !g_PollHook)
        {
            spdlog::warn("Mouse input: hooks failed (register {}, poll {}); the game keeps its own mouse path.",
                g_RegisterHook ? "ok" : "FAILED", g_PollHook ? "ok" : "FAILED");
            g_PollHook = {};
            g_RegisterHook = {};
            return;
        }
        const auto base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        spdlog::info("Mouse input: GLFW at pollEvents +{:X}, cursorPos +{:X}, click +{:X}, scroll +{:X}; windows list +{:X}, disabled window +{:X}.",
            reinterpret_cast<uintptr_t>(poll) - base, reinterpret_cast<uintptr_t>(cursorPos) - base, reinterpret_cast<uintptr_t>(g_Click) - base,
            reinterpret_cast<uintptr_t>(scroll) - base, reinterpret_cast<uintptr_t>(g_WindowList) - base, reinterpret_cast<uintptr_t>(g_DisabledWindow) - base);
        if (const HANDLE h = CreateThread(nullptr, 0, ReaderThread, nullptr, 0, nullptr)) { CloseHandle(h); }
    }
}
