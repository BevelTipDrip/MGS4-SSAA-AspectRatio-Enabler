#include "pch.hpp"
#include "input_probe.hpp"

#include "game.hpp"
#include "mouse_aim.hpp"
#include "busy_wait.hpp"
#include "sites.hpp"

#include <intrin.h>
#include <thread>

namespace
{
    // Function starts, found by signature (made with the harness's sig_one.py; each is unique in
    // the 2026-09-18 build and in the one before it).
    constexpr mgspwe::sites::Signature kRawMouseSig { "input: raw mouse handler", 0x3A390, "66 83 39 00 75 4C 8B 41 10 66 0F 6E 49 0C", 0 };
    constexpr mgspwe::sites::Signature kUpdateSig { "input: per-frame device update", 0x3A850, "40 53 48 83 EC 20 48 8B D9 48 8D 41 15", 0 };
    constexpr mgspwe::sites::Signature kGetterSig { "input: value getter", 0x3A9F0, "4C 8B C1 F7 C2 00 00 FF FF 75 28 80 79 09 00", 0 };
    // Action::Update: for each of two players, the largest value over the action's bindings
    // (each evaluated by +2D300, which is what calls the getter). Per player, at this + 0x68 +
    // player * 0x24: +0 current, +4 previous, +0x10 the value reported to the game (key repeat
    // applied), +0x14 / +0x18 press and release timers, +0x1C the source kind.
    constexpr mgspwe::sites::Signature kActionUpdateSig { "input: action update", 0x2C720, "48 89 5C 24 08 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 40", 0 };
    constexpr size_t kActionCurrent = 0x68, kActionReported = 0x78;
    // The look-input routine (+73D37E) storing look X into the player: movss [rbx+0x998], xmm8.
    // Its time inside the tick, against the camera routines', says whether the camera reads this
    // tick's value or last tick's.
    constexpr mgspwe::sites::Signature kLookStoreSig { "input: look X store", 0x73EBD2, "F3 44 0F 11 83 98 09 00 00 8B 47 70", 0 };

    // The getter's mouse codes are 0x10001 .. 0x1000D, indexed from 0x10001 by its jump table:
    // 0, 1, 3, 4, 5 buttons; 7 Y+ (0x10008), 8 Y- (0x10009), 9 X- (0x1000A), 10 X+ (0x1000B);
    // 11 and 12 the wheel's two directions.
    constexpr uint32_t kMouseCodeBase = 0x10001;
    constexpr int kMouseCodes = 13;
    constexpr uint32_t kFirstDirection = 0x10008;
    const char* const kDirectionNames[4] = { "Y+", "Y-", "X-", "X+" };

    SafetyHookInline g_RawMouse, g_Update, g_Getter, g_ActionUpdate;
    SafetyHookMid g_LookStore;
    uint64_t g_LookSerial = 0, g_LookSerialSeen = 0, g_CameraSerialSeen = 0, g_LateSerialSeen = 0;
    float g_LookY = 0;
    // When inside the tick the raw mouse handler runs: milliseconds since the last latch, 1 ms bins
    // (the last bin takes everything later), and on which threads.
    std::array<std::atomic<uint32_t>, 24> g_HandlerBins {};
    std::atomic<int64_t> g_LatchQpcShared { 0 };
    // Spacing between consecutive handler calls while the mouse is moving (gaps over 50 ms are
    // pauses, not spacing): a 1000 Hz source handled promptly puts everything in the 0.5 - 1.5 ms
    // bins; calls that come in clumps after a sleep show as a pile under 0.25 ms plus a tail.
    constexpr double kGapEdges[] = { 0.25, 0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0, 12.0 };
    std::array<std::atomic<uint32_t>, 11> g_GapBins {};
    std::atomic<int64_t> g_LastHandlerQpc { 0 };
    std::atomic<uint32_t> g_HandlerThread { 0 }, g_OtherHandlerThread { 0 };
    uint32_t g_GameThreadId = 0;
    int64_t g_LookQpc = 0;
    float g_LookX = 0;
    double g_QpcToMs = 0;
    const float* g_AccumX = nullptr;
    const float* g_AccumY = nullptr;

    // Written by the raw handler, taken by the device update.
    std::atomic<int64_t> g_Events { 0 }, g_SumDx { 0 }, g_SumDy { 0 }, g_FirstQpc { 0 }, g_LastQpc { 0 };

    // Per frame, getter side. Only the game thread touches these.
    struct CodeStat { int calls; float last; float peak; };
    CodeStat g_Codes[kMouseCodes] {};
    uint64_t g_Frame = 0;
    int g_QuietFrames = 1000;
    int g_ActiveFrames = 0;
    int g_Lines = 0;
    constexpr int kMaxLines = 40000;

    struct Caller { uintptr_t rva; uint64_t calls; };
    std::array<Caller, 32> g_Callers {};
    int g_CallerCount = 0;

    // The actions that ask for a mouse direction, found as they ask. Game thread only.
    struct TrackedAction { const uint8_t* action; uint32_t code; float current; float reported; };
    std::array<TrackedAction, 64> g_Actions {};   // 16 was too few (2026-09-19): a fourth X pair was found after the table filled
    int g_ActionCount = 0;
    const uint8_t* g_CurrentAction = nullptr;

    // Hardware data breakpoints on the CURRENT value of up to four tracked actions: whatever
    // reads them is the consumer (the camera side). The reported value (+0x78) was watched first
    // (2026-09-19): it follows a key-repeat rule (first frame, nothing for 18 frames, then one
    // frame in six) and nothing but the action's own update touched it while the camera turned.
    // Armed once, from a helper thread, on the game thread; a vectored handler logs each distinct
    // accessing instruction, per watched action, with the code addresses found on the stack.
    HANDLE g_GameThread = nullptr;
    std::atomic<bool> g_WatchArmed { false };
    uintptr_t g_Watched[4] {};
    uint32_t g_WatchedCode[4] {};
    uint32_t g_WatchedName[4] {};
    std::array<std::atomic<uint32_t>, 4> g_SlotAccesses {};   // outside accesses since the last once-a-second line
    std::array<std::atomic<uintptr_t>, 96> g_AccessRips {};   // (slot << 56) | instruction address
    std::atomic<int> g_AccessCount { 0 };
    std::atomic<uint64_t> g_OwnAccesses { 0 };                // from inside the action's own update: counted, not logged
    uintptr_t g_ActionUpdateStart = 0, g_ActionUpdateEnd = 0;

    int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

    uintptr_t Rva(void* address)
    {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
        const uintptr_t a = reinterpret_cast<uintptr_t>(address);
        return a >= base ? a - base : a;
    }

    void Hooked_RawMouse(const RAWMOUSE* mouse)
    {
        if (mouse && mouse->usFlags == 0 && (mouse->lLastX != 0 || mouse->lLastY != 0))
        {
            const int64_t now = Qpc();
            const uint32_t tid = GetCurrentThreadId();
            uint32_t expected = 0;
            if (!g_HandlerThread.compare_exchange_strong(expected, tid) && expected != tid) { g_OtherHandlerThread = tid; }
            const int64_t previous = g_LastHandlerQpc.exchange(now);
            if (previous != 0 && g_QpcToMs != 0)
            {
                const double gap = (now - previous) * g_QpcToMs;
                if (gap < 50.0)
                {
                    size_t bin = 0;
                    while (bin < std::size(kGapEdges) && gap >= kGapEdges[bin]) { bin++; }
                    g_GapBins[bin]++;
                }
            }
            const int64_t latch = g_LatchQpcShared.load();
            if (latch != 0 && g_QpcToMs != 0)
            {
                const double ms = (now - latch) * g_QpcToMs;
                g_HandlerBins[static_cast<size_t>(std::min(23.0, std::max(0.0, ms)))]++;
            }
            if (g_Events.fetch_add(1) == 0) { g_FirstQpc = now; }
            g_LastQpc = now;
            g_SumDx += mouse->lLastX;
            g_SumDy += mouse->lLastY;
        }
        g_RawMouse.call<void>(mouse);
    }

    void NoteCaller(void* returnAddress)
    {
        const uintptr_t rva = Rva(returnAddress);
        for (int i = 0; i < g_CallerCount; i++) { if (g_Callers[i].rva == rva) { g_Callers[i].calls++; return; } }
        if (g_CallerCount >= static_cast<int>(g_Callers.size())) { return; }
        g_Callers[g_CallerCount++] = { rva, 1 };
        void* frames[10] {};
        const USHORT n = RtlCaptureStackBackTrace(1, 10, frames, nullptr);
        std::string stack;
        for (USHORT i = 0; i < n; i++) { stack += fmt::format(" +{:X}", Rva(frames[i])); }
        spdlog::info("PW input: new caller of the value getter for a mouse code: returns to +{:X}; stack:{}", rva, stack);
    }

    void NoteAction(uint32_t code)
    {
        const uint8_t* action = g_CurrentAction;
        if (!action) { return; }
        for (int i = 0; i < g_ActionCount; i++) { if (g_Actions[i].action == action && g_Actions[i].code == code) { return; } }
        if (g_ActionCount >= static_cast<int>(g_Actions.size())) { return; }
        g_Actions[g_ActionCount++] = { action, code, 0, 0 };
        std::string head;
        for (int i = 0; i < 0x68; i += 4) { uint32_t v; std::memcpy(&v, action + i, 4); head += fmt::format(" {:08X}", v); }
        uint32_t name = 0, group = 0; std::memcpy(&name, action + 4, 4); std::memcpy(&group, action + 8, 4);
        spdlog::info("PW input: action #{} {:p} name {:#08x} group {:#08x} asks for mouse {} ({:#x}); first 0x68 bytes:{}",
            g_ActionCount - 1, static_cast<const void*>(action), name, group, kDirectionNames[code - kFirstDirection], code, head);
    }

    float Hooked_Getter(void* self, uint32_t code)
    {
        const float value = g_Getter.call<float>(self, code);
        const uint32_t index = code - kMouseCodeBase;
        if (index < static_cast<uint32_t>(kMouseCodes))
        {
            CodeStat& c = g_Codes[index];
            c.calls++;
            c.last = value;
            c.peak = std::max(c.peak, value);
            if (index >= 7 && index <= 10) { NoteCaller(_ReturnAddress()); NoteAction(code); }
        }
        return value;
    }

    uintptr_t Hooked_ActionUpdate(void* self)
    {
        const uint8_t* previous = g_CurrentAction;
        g_CurrentAction = static_cast<const uint8_t*>(self);
        const uintptr_t result = g_ActionUpdate.call<uintptr_t>(self);
        g_CurrentAction = previous;
        if (!g_WatchArmed)   // once armed, reading the watched fields from here would only trip the breakpoints
        {
            for (int i = 0; i < g_ActionCount; i++)
            {
                if (g_Actions[i].action != self) { continue; }
                std::memcpy(&g_Actions[i].current, g_Actions[i].action + kActionCurrent, 4);
                std::memcpy(&g_Actions[i].reported, g_Actions[i].action + kActionReported, 4);
            }
        }
        return result;
    }

    LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* info)
    {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || (info->ContextRecord->Dr6 & 0xF) == 0) { return EXCEPTION_CONTINUE_SEARCH; }
        const uintptr_t hit = info->ContextRecord->Dr6 & 0xF;
        info->ContextRecord->Dr6 = 0;
        const uintptr_t rip = info->ContextRecord->Rip;   // a data breakpoint traps after the access: this is the next instruction
        if (rip >= g_ActionUpdateStart && rip <= g_ActionUpdateEnd) { g_OwnAccesses++; return EXCEPTION_CONTINUE_EXECUTION; }
        int which = 0;
        while (which < 3 && !(hit & (uintptr_t(1) << which))) { which++; }
        g_SlotAccesses[which]++;
        const uintptr_t key = (uintptr_t(which) << 56) | rip;
        const int n = g_AccessCount.load();
        bool known = false;
        for (int i = 0; i < n; i++) { known = known || g_AccessRips[i] == key; }
        if (!known && n < static_cast<int>(g_AccessRips.size()))
        {
            g_AccessRips[n] = key;
            g_AccessCount = n + 1;
            // Return addresses straight off the stack: anything that points into the game's code.
            const uintptr_t base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
            const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(info->ContextRecord->Rsp);
            std::string stack;
            int found = 0;
            for (int i = 0; i < 96 && found < 10; i++)
            {
                const uintptr_t v = sp[i];
                if (v > base + 0x1000 && v < base + 0x98D000) { stack += fmt::format(" +{:X}", v - base); found++; }
            }
            uint32_t name = 0; std::memcpy(&name, reinterpret_cast<const uint8_t*>(g_Watched[which] - kActionCurrent) + 4, 4);
            spdlog::info("PW input: watch: the current value of action {:#08x} (mouse {}, slot {}) was accessed by the instruction before +{:X}; code addresses on the stack:{}",
                name, kDirectionNames[g_WatchedCode[which] - kFirstDirection], which, rip - base, stack);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void ArmWatch()
    {
        // By name: X+ and Y+ of the camera group 0x1512DE (read by the look-input routine +73D37E)
        // and of the second group 0x43A215, whose use is the open question (aiming down the sights
        // is the user's suggestion). The opposite directions are read by the same code.
        int slots = 0;
        for (const uint32_t wantName : { 0xBC4D79u, 0x68F387u, 0x480BDCu, 0x942735u })
        {
            for (int i = 0; i < g_ActionCount && slots < 4; i++)
            {
                uint32_t name = 0; std::memcpy(&name, g_Actions[i].action + 4, 4);
                if (name != wantName || (g_Actions[i].code != 0x1000Bu && g_Actions[i].code != 0x10008u)) { continue; }
                g_Watched[slots] = reinterpret_cast<uintptr_t>(g_Actions[i].action) + kActionCurrent;
                g_WatchedCode[slots] = g_Actions[i].code;
                g_WatchedName[slots] = name;
                slots++;
                break;
            }
        }
        if (slots == 0 || !g_GameThread) { return; }
        g_WatchArmed = true;
        AddVectoredExceptionHandler(1, WatchHandler);
        std::thread([slots]
        {
            if (SuspendThread(g_GameThread) == static_cast<DWORD>(-1)) { spdlog::warn("PW input: watch: the game thread could not be suspended."); return; }
            CONTEXT context {};
            context.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            bool ok = GetThreadContext(g_GameThread, &context) != 0;
            if (ok)
            {
                DWORD64* registers[4] = { &context.Dr0, &context.Dr1, &context.Dr2, &context.Dr3 };
                DWORD64 control = 0;
                for (int i = 0; i < slots; i++)
                {
                    *registers[i] = g_Watched[i];
                    control |= DWORD64(1) << (i * 2);          // local enable
                    control |= DWORD64(0xF) << (16 + i * 4);   // read or write, four bytes
                }
                context.Dr6 = 0;
                context.Dr7 = control;
                ok = SetThreadContext(g_GameThread, &context) != 0;
            }
            ResumeThread(g_GameThread);
            spdlog::info("PW input: watch: {} hardware breakpoint(s) on current action values: {}.", slots, ok ? "armed" : "FAILED");
        }).detach();
    }

    // One line for the frame that just ended (its getter answers), then the latch for the new one.
    float g_LatchedX = 0, g_LatchedY = 0;
    int64_t g_LatchEvents = 0, g_LatchDx = 0, g_LatchDy = 0, g_LatchFirst = 0, g_LatchLast = 0, g_LatchQpc = 0;

    void FlushFrame()
    {
        bool active = g_LatchEvents != 0 || g_LatchedX != 0 || g_LatchedY != 0;
        for (int i = 7; i <= 12; i++) { active = active || g_Codes[i].peak != 0; }
        {
            // With the late sample on, the camera can turn in a frame whose latch saw nothing yet.
            const MouseAim::LateSample& late = MouseAim::LastLate();
            const MouseAim::CameraSample& camera = MouseAim::LastCamera();
            active = active || (late.serial != g_LateSerialSeen && (late.x != 0 || late.y != 0));
            active = active || (camera.serial != g_CameraSerialSeen && (camera.addedYaw != 0 || camera.addedPitch != 0));
            g_LateSerialSeen = late.serial;
        }
        g_QuietFrames = active ? 0 : g_QuietFrames + 1;
        if (g_QuietFrames <= 45 && g_Lines < kMaxLines)   // the movement and three quarters of a second after it
        {
            g_Lines++;
            spdlog::info("PW input: f={} qpc={} ev={} raw=({},{}) first={} last={} latched=({:.5f},{:.5f}) get X+={:.5f}x{} X-={:.5f}x{} Y+={:.5f}x{} Y-={:.5f}x{}",
                g_Frame, g_LatchQpc, g_LatchEvents, g_LatchDx, g_LatchDy, g_LatchFirst, g_LatchLast, g_LatchedX, g_LatchedY,
                g_Codes[10].last, g_Codes[10].calls, g_Codes[9].last, g_Codes[9].calls, g_Codes[7].last, g_Codes[7].calls, g_Codes[8].last, g_Codes[8].calls);
            if (g_Lines == kMaxLines) { spdlog::info("PW input: line cap reached; no more frame lines."); }
            // The same tick, further down: when the look value was stored, when a camera routine
            // turned the angles, and with what. Times are milliseconds after this tick's latch.
            const MouseAim::CameraSample& cam = MouseAim::LastCamera();
            const bool lookRan = g_LookSerial != g_LookSerialSeen, cameraRan = cam.serial != g_CameraSerialSeen;
            if (lookRan || cameraRan)
            {
                spdlog::info("PW input: f={} tick: look store {} (look X {:.5f} Y {:.5f}); camera {} {} mouse={} turn=({:.4f},{:.4f}) added=({},{}) carry=({:.4f},{:.4f}) pitch/yaw before={}/{}; late sample: {} msg, ({:.5f},{:.5f}) {} in {:.3f} ms",
                    g_Frame,
                    lookRan ? fmt::format("+{:.3f} ms", (g_LookQpc - g_LatchQpc) * g_QpcToMs) : std::string("did not run"), g_LookX, g_LookY,
                    cameraRan ? (cam.routine == 1 ? "A" : cam.routine == 2 ? "B" : "F") : "-", cameraRan ? fmt::format("+{:.3f} ms", (cam.qpc - g_LatchQpc) * g_QpcToMs) : std::string("did not run"),
                    cam.mouse ? 1 : 0, cam.velocityPitch, cam.velocityYaw, cam.addedPitch, cam.addedYaw, cam.carryPitch, cam.carryYaw, cam.pitchBefore, cam.yawBefore,
                    MouseAim::LastLate().messages, MouseAim::LastLate().x, MouseAim::LastLate().y, MouseAim::LastLate().applied ? "applied" : "-", MouseAim::LastLate().ticks * g_QpcToMs);
            }
            if (!g_WatchArmed && g_ActionCount > 0)
            {
                std::string actions;
                for (int i = 0; i < g_ActionCount; i++)
                {
                    if (g_Actions[i].code != 0x1000Bu && g_Actions[i].code != 0x10008u) { continue; }
                    uint32_t name = 0; std::memcpy(&name, g_Actions[i].action + 4, 4);
                    actions += fmt::format(" {:06X}:{}={:.5f}", name, kDirectionNames[g_Actions[i].code - kFirstDirection], g_Actions[i].current);
                }
                spdlog::info("PW input: f={} actions (current/reported):{}", g_Frame, actions);
            }
        }
        g_LookSerialSeen = g_LookSerial;
        g_CameraSerialSeen = MouseAim::LastCamera().serial;
        // Thirty frames of movement are logged with the action values, then the breakpoints take over.
        if (active) { g_ActiveFrames++; }
        if (InputProbe::bWatchActions && !g_WatchArmed && g_ActiveFrames == 30) { ArmWatch(); }
        for (CodeStat& c : g_Codes) { c = {}; }
    }

    void DumpHistograms()
    {
        std::string bins;
        for (size_t i = 0; i < g_HandlerBins.size(); i++) { bins += fmt::format(" {}", g_HandlerBins[i].exchange(0)); }
        spdlog::info("PW input: raw mouse handler calls by millisecond since the latch (0 .. 23+):{}; handler thread {}, another handler thread {}, game thread {}.",
            bins, g_HandlerThread.load(), g_OtherHandlerThread.load(), g_GameThreadId);
        std::string gaps;
        for (size_t i = 0; i < g_GapBins.size(); i++) { gaps += fmt::format(" {}", g_GapBins[i].exchange(0)); }
        spdlog::info("PW input: spacing between handler calls, ms bins <0.25 <0.5 <1 <1.5 <2 <3 <4 <6 <8 <12 >=12:{}", gaps);
    }

    // Live switches, so one session compares states at the same spot. A beep says what happened
    // without a look at the log: high = on, low = off; one, two or three beeps = which fix.
    //   F6 fraction carry   F7 late sample   F8 prompt pump   F9 all three   F10 histograms
    void Announce(int beeps, bool on)
    {
        std::thread([beeps, on] { for (int i = 0; i < beeps; i++) { Beep(on ? 1400 : 500, 70); Sleep(60); } }).detach();
    }

    bool Pressed(int key, bool& wasDown)
    {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool edge = down && !wasDown;
        wasDown = down;
        return edge;
    }

    void PollHotkeys()
    {
        static bool d6 = false, d7 = false, d8 = false, d9 = false, d10 = false;
        bool changed = false;
        if (Pressed(VK_F6, d6)) { MouseAim::bFractionCarry = !MouseAim::bFractionCarry; Announce(1, MouseAim::bFractionCarry); changed = true; }
        if (Pressed(VK_F7, d7)) { MouseAim::bLateSample = !MouseAim::bLateSample; Announce(2, MouseAim::bLateSample); changed = true; }
        if (Pressed(VK_F8, d8)) { BusyWait::bPromptPump = !BusyWait::bPromptPump; Announce(3, BusyWait::bPromptPump); changed = true; }
        if (Pressed(VK_F9, d9))
        {
            const bool on = !(MouseAim::bFractionCarry && MouseAim::bLateSample && BusyWait::bPromptPump);
            MouseAim::bFractionCarry = MouseAim::bLateSample = BusyWait::bPromptPump = on;
            Announce(4, on);
            changed = true;
        }
        if (changed)
        {
            spdlog::info("PW input: hotkey at f={}: fraction carry {}, late sample {}, prompt pump {}.", g_Frame,
                MouseAim::bFractionCarry ? "ON" : "off", MouseAim::bLateSample ? "ON" : "off", BusyWait::bPromptPump ? "ON" : "off");
            DumpHistograms();
        }
        if (Pressed(VK_F10, d10))
        {
            spdlog::info("PW input: F10 at f={}: histograms (fraction carry {}, late sample {}, prompt pump {}).", g_Frame,
                MouseAim::bFractionCarry ? "ON" : "off", MouseAim::bLateSample ? "ON" : "off", BusyWait::bPromptPump ? "ON" : "off");
            DumpHistograms();
        }
    }

    uintptr_t Hooked_Update(void* self)
    {
        if (g_Frame != 0) { FlushFrame(); }
        g_Frame++;
        PollHotkeys();
        // What the game is about to latch, read before it zeroes the accumulators.
        g_LatchQpc = Qpc();
        g_LatchQpcShared = g_LatchQpc;
        g_LatchedX = *g_AccumX;
        g_LatchedY = *g_AccumY;
        g_LatchEvents = g_Events.exchange(0);
        g_LatchDx = g_SumDx.exchange(0);
        g_LatchDy = g_SumDy.exchange(0);
        g_LatchFirst = g_FirstQpc.load();
        g_LatchLast = g_LastQpc.load();
        if (g_Frame == 1)
        {
            DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_GameThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
            g_GameThreadId = GetCurrentThreadId();
            LARGE_INTEGER f; QueryPerformanceFrequency(&f);
            g_QpcToMs = 1000.0 / static_cast<double>(f.QuadPart);
            spdlog::info("PW input: first device update; device {:p}, QPC frequency {}.", self, f.QuadPart);
        }
        if (g_WatchArmed && g_Frame % 60 == 0)
        {
            // Once a second: who is being read right now. With the user naming the mode they are in
            // (free camera, aiming, scope, menus), the time stamps say which group serves which.
            uint32_t n[4]; bool any = false;
            for (int i = 0; i < 4; i++) { n[i] = g_SlotAccesses[i].exchange(0); any = any || n[i] != 0; }
            if (any)
            {
                spdlog::info("PW input: watch: reads in the last second: {:06X} {}, {:06X} {}, {:06X} {}, {:06X} {}.",
                    g_WatchedName[0], n[0], g_WatchedName[1], n[1], g_WatchedName[2], n[2], g_WatchedName[3], n[3]);
            }
        }
        if (g_Frame % 3600 == 0)
        {
            std::string callers;
            for (int i = 0; i < g_CallerCount; i++) { callers += fmt::format(" +{:X}:{}", g_Callers[i].rva, g_Callers[i].calls); }
            spdlog::info("PW input: {} device updates; getter callers for the mouse directions:{}; watch: {} distinct outside accesses, {} from the action update itself.",
                g_Frame, callers.empty() ? " none yet" : callers, g_AccessCount.load(), g_OwnAccesses.load());
        }
        return g_Update.call<uintptr_t>(self);
    }
}

void InputProbe::Install()
{
    if (!bEnabled) { return; }
    MouseAim::bTelemetry = true;   // MouseAim::Install runs after this and hooks the camera sites for the numbers
    const uintptr_t base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());
    const uintptr_t rawMouse = mgspwe::sites::Resolve(kRawMouseSig);
    const uintptr_t update = mgspwe::sites::Resolve(kUpdateSig);
    const uintptr_t getter = mgspwe::sites::Resolve(kGetterSig);
    const uintptr_t actionUpdate = mgspwe::sites::Resolve(kActionUpdateSig);
    if (!rawMouse || !update || !getter || !actionUpdate) { spdlog::warn("PW input: a site was not found; the probe stays off."); return; }

    // The accumulators' addresses come from the handler's own instructions:
    //   +0x10  F3 0F 10 15 disp32   movss xmm2, [accumX]
    //   +0x18  F3 0F 10 05 disp32   movss xmm0, [accumY]
    const uint8_t* code = reinterpret_cast<const uint8_t*>(base + rawMouse);
    const uint8_t loadX[] = { 0xF3, 0x0F, 0x10, 0x15 }, loadY[] = { 0xF3, 0x0F, 0x10, 0x05 };
    if (std::memcmp(code + 0x10, loadX, 4) != 0 || std::memcmp(code + 0x18, loadY, 4) != 0)
    {
        spdlog::warn("PW input: the raw mouse handler does not load its accumulators where expected; the probe stays off.");
        return;
    }
    int32_t dispX = 0, dispY = 0;
    std::memcpy(&dispX, code + 0x14, 4);
    std::memcpy(&dispY, code + 0x1C, 4);
    g_AccumX = reinterpret_cast<const float*>(code + 0x18 + dispX);
    g_AccumY = reinterpret_cast<const float*>(code + 0x20 + dispY);

    g_RawMouse = safetyhook::create_inline(reinterpret_cast<void*>(base + rawMouse), reinterpret_cast<void*>(Hooked_RawMouse));
    g_Update = safetyhook::create_inline(reinterpret_cast<void*>(base + update), reinterpret_cast<void*>(Hooked_Update));
    g_Getter = safetyhook::create_inline(reinterpret_cast<void*>(base + getter), reinterpret_cast<void*>(Hooked_Getter));
    g_ActionUpdate = safetyhook::create_inline(reinterpret_cast<void*>(base + actionUpdate), reinterpret_cast<void*>(Hooked_ActionUpdate));
    if (const uintptr_t look = mgspwe::sites::Resolve(kLookStoreSig))
    {
        g_LookStore = safetyhook::create_mid(base + look, [](SafetyHookContext& ctx)
        {
            g_LookSerial++;
            g_LookQpc = Qpc();
            g_LookX = ctx.xmm8.f32[0];
            std::memcpy(&g_LookY, reinterpret_cast<const uint8_t*>(ctx.rdi) + 0x70, 4);   // stored to player + 0x99C by the next two instructions
        });
        spdlog::info("PW input: look X store +{:X} {}.", look, g_LookStore ? "hooked" : "FAILED");
    }
    g_ActionUpdateStart = base + actionUpdate;
    g_ActionUpdateEnd = base + actionUpdate + 0x1B2;   // the function is 434 bytes (.pdata)
    spdlog::info("PW input: probe installed: raw mouse handler +{:X} {}, device update +{:X} {}, value getter +{:X} {}, action update +{:X} {}; accumulators at +{:X} and +{:X}.",
        rawMouse, g_RawMouse ? "ok" : "FAILED", update, g_Update ? "ok" : "FAILED", getter, g_Getter ? "ok" : "FAILED", actionUpdate, g_ActionUpdate ? "ok" : "FAILED",
        reinterpret_cast<uintptr_t>(g_AccumX) - base, reinterpret_cast<uintptr_t>(g_AccumY) - base);
}
