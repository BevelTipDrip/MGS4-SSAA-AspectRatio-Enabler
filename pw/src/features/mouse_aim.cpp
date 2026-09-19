#include "pch.hpp"
#include "mouse_aim.hpp"

#include "game.hpp"
#include "sites.hpp"

namespace
{
    // Signatures made with the harness's sig_one.py; each is unique in the 2026-09-18 build and in
    // the one before it.
    constexpr mgspwe::sites::Signature kRawMouseSig { "mouse: raw mouse handler", 0x3A390, "66 83 39 00 75 4C 8B 41 10 66 0F 6E 49 0C", 0 };
    // The look-input routine, right before it looks up the four mouse-look actions of group
    // 0x1512DE: mov rcx, [input manager]; xor r9d, r9d; mov edx, 0x1512DE; mov r8d, 0xBC4D79; call lookup.
    constexpr mgspwe::sites::Signature kLookInputSig { "mouse: look input, before the four lookups", 0x73E78A, "48 8B 0D ?? ?? ?? ?? 45 33 C9 BA DE 12 15 00 41 B8 79 4D BC 00 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ??", 0 };
    // float* Lookup(manager, group, name, player): pointer to the action's current value, with the
    // source kind at +0x1C and +0x20. Disabled input returns a pointer to a static zero block
    // inside the image (+15FF210), which must never be written.
    constexpr mgspwe::sites::Signature kLookupSig { "mouse: action lookup", 0x2B010, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 49 63 D9", 0 };
    // Turn truncations: cvttss2si, then add word [camera + angle].
    //   free camera yaw   +4042B6  cvttss2si eax, xmm6              camera = rbx, yaw at +0x36C, mouse flag at +0x348
    //   camera A          +885C5E  cvttss2si eax, xmm6 (pitch) ... ecx, xmm3 (yaw)   player = rdi, camera = rbx
    //   camera B          +8894A3  cvttss2si ecx, [rbx+0xBC] (yaw) ... eax, xmm6 (pitch)
    // The free camera's vertical control (+4039F0) keeps a float and loses nothing.
    constexpr mgspwe::sites::Signature kTruncateFreeSig { "mouse: free camera yaw truncation", 0x4042B6, "F3 0F 2C C6 BA 00 00 00 04 48 8B CB", 0 };
    constexpr mgspwe::sites::Signature kTruncateASig { "mouse: camera A turn truncation", 0x885C5E, "F3 0F 2C C6 48 8D 54 24 28 C7 44 24 2C 00 00 00 00", 0 };
    constexpr mgspwe::sites::Signature kTruncateBSig { "mouse: camera B turn truncation", 0x8894A3, "F3 0F 2C 8B BC 00 00 00 48 8D 54 24 20", 0 };

    constexpr size_t kPlayerIsMouse = 0x9B2;
    constexpr size_t kCameraPitch = 0xA0, kCameraYaw = 0xA2, kCameraVelocityYaw = 0xBC;
    constexpr size_t kFreeCameraYaw = 0x36C, kFreeCameraIsMouse = 0x348;

    constexpr uint32_t kLookGroup = 0x1512DE;
    constexpr uint32_t kLookRight = 0xBC4D79, kLookLeft = 0x2AD254, kLookUp = 0x480BDC, kLookDown = 0x26FC6E;   // X+, X-, Y+, Y-
    constexpr size_t kActionKind = 0x1C, kActionKind2 = 0x20;   // from the pointer to the current value; 1 and 1 = the mouse

    using LookupFn = float* (*)(void* manager, uint32_t group, uint32_t name, int player);

    SafetyHookMid g_LookInput, g_TruncateFree, g_TruncateA, g_TruncateB;
    LookupFn g_Lookup = nullptr;
    void** g_Manager = nullptr;
    float* g_AccumX = nullptr;
    float* g_AccumY = nullptr;
    uintptr_t g_ImageStart = 0, g_ImageEnd = 0;

    MouseAim::CameraSample g_Camera;
    MouseAim::LateSample g_Late;
    float g_CarryPitch = 0, g_CarryYaw = 0;

    int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

    // The whole number the game is to add, and the remainder kept for the next frame. Truncation
    // toward zero, as the game does it, so that a carry of zero reproduces the game exactly.
    int Carry(float velocity, float& carry)
    {
        const float exact = velocity + carry;
        const float whole = std::trunc(exact);
        carry = exact - whole;
        return static_cast<int>(whole);
    }

    void OnTruncate(int routine, bool mouse, uint16_t pitchBefore, uint16_t yawBefore, float* pitch, float& yaw)
    {
        if (MouseAim::bTelemetry)
        {
            g_Camera.serial++;
            g_Camera.routine = routine;
            g_Camera.mouse = mouse;
            g_Camera.velocityPitch = pitch ? *pitch : 0;
            g_Camera.velocityYaw = yaw;
            g_Camera.pitchBefore = pitchBefore;
            g_Camera.yawBefore = yawBefore;
            g_Camera.qpc = Qpc();
        }
        if (MouseAim::bFractionCarry && mouse)
        {
            if (pitch) { *pitch = static_cast<float>(Carry(*pitch, g_CarryPitch)); }
            yaw = static_cast<float>(Carry(yaw, g_CarryYaw));
        }
        else if (!mouse)
        {
            g_CarryPitch = g_CarryYaw = 0;   // a stick frame in between: nothing of the mouse's is owed any more
        }
        if (MouseAim::bTelemetry)
        {
            g_Camera.addedPitch = pitch ? static_cast<int>(*pitch) : 0;
            g_Camera.addedYaw = static_cast<int>(yaw);
            g_Camera.carryPitch = g_CarryPitch;
            g_Camera.carryYaw = g_CarryYaw;
        }
    }

    uint16_t Word(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }

    // The game latches the mouse (device update, +3A850) and only then runs the frame, whose wait
    // comes before the player's look-input routine: measured 16.3 ms between the latch and the
    // look value, so the mouse is a frame old when it is used, and 16 ms of newer movement is
    // sitting unread in the message queue. Here, right before the routine reads its four actions:
    // dispatch the pending raw input (the game's own window procedure and handler accumulate it),
    // fold what has accumulated into the four actions, and clear the accumulators so the next
    // latch does not count it again. Every count is used once, just a frame earlier.
    void SampleLate()
    {
        const int64_t start = Qpc();
        // The W calls on purpose: the busy wait fix hooks PeekMessageA and parks there when the
        // queue is empty. WM_INPUT carries no text, so the game's ANSI window procedure gets it unchanged.
        MSG message;
        int n = 0;
        while (n < 8192 && PeekMessageW(&message, nullptr, WM_INPUT, WM_INPUT, PM_REMOVE))
        {
            DispatchMessageW(&message);
            n++;
        }
        const float x = *g_AccumX, y = *g_AccumY;
        g_Late.serial++;
        g_Late.messages = n;
        g_Late.x = x;
        g_Late.y = y;
        g_Late.applied = false;
        if (x != 0 || y != 0)
        {
            void* manager = *g_Manager;
            float* targets[2] = {
                manager ? g_Lookup(manager, kLookGroup, x > 0 ? kLookRight : kLookLeft, 0) : nullptr,
                manager ? g_Lookup(manager, kLookGroup, y > 0 ? kLookUp : kLookDown, 0) : nullptr };
            const float amounts[2] = { std::fabs(x), std::fabs(y) };
            bool ok = true;
            for (float* t : targets)
            {
                const uintptr_t a = reinterpret_cast<uintptr_t>(t);
                ok = ok && t && !(a >= g_ImageStart && a < g_ImageEnd);   // inside the image = the static zero block of disabled input
            }
            if (ok)
            {
                for (int i = 0; i < 2; i++)
                {
                    if (amounts[i] == 0) { continue; }
                    *targets[i] += amounts[i];
                    uint32_t one = 1;
                    std::memcpy(reinterpret_cast<uint8_t*>(targets[i]) + kActionKind, &one, 4);
                    std::memcpy(reinterpret_cast<uint8_t*>(targets[i]) + kActionKind2, &one, 4);
                }
                *g_AccumX = 0;
                *g_AccumY = 0;
                g_Late.applied = true;
            }
        }
        g_Late.ticks = Qpc() - start;
    }
}

const MouseAim::CameraSample& MouseAim::LastCamera() { return g_Camera; }
const MouseAim::LateSample& MouseAim::LastLate() { return g_Late; }

void MouseAim::Install()
{
    if (!bFractionCarry && !bLateSample && !bTelemetry) { return; }
    const uintptr_t base = reinterpret_cast<uintptr_t>(mgs4e::game::Module());

    if (bLateSample || bTelemetry)   // with the probe on, F9 switches the fixes live, so the hook has to be there
    {
        const uintptr_t handler = mgspwe::sites::Resolve(kRawMouseSig);
        const uintptr_t look = mgspwe::sites::Resolve(kLookInputSig);
        const uintptr_t lookup = mgspwe::sites::Resolve(kLookupSig);
        bool ok = handler && look && lookup;
        if (ok)
        {
            // The accumulators, from the handler's own instructions (+0x10 movss xmm2,[accumX], +0x18 movss xmm0,[accumY]).
            const uint8_t* code = reinterpret_cast<const uint8_t*>(base + handler);
            const uint8_t loadX[] = { 0xF3, 0x0F, 0x10, 0x15 }, loadY[] = { 0xF3, 0x0F, 0x10, 0x05 };
            ok = std::memcmp(code + 0x10, loadX, 4) == 0 && std::memcmp(code + 0x18, loadY, 4) == 0;
            if (ok)
            {
                int32_t dx = 0, dy = 0, dm = 0;
                std::memcpy(&dx, code + 0x14, 4);
                std::memcpy(&dy, code + 0x1C, 4);
                g_AccumX = const_cast<float*>(reinterpret_cast<const float*>(code + 0x18 + dx));
                g_AccumY = const_cast<float*>(reinterpret_cast<const float*>(code + 0x20 + dy));
                // The input manager's global, from the hooked instruction itself: 48 8B 0D disp32.
                const uint8_t* site = reinterpret_cast<const uint8_t*>(base + look);
                std::memcpy(&dm, site + 3, 4);
                g_Manager = const_cast<void**>(reinterpret_cast<void* const*>(site + 7 + dm));
                g_Lookup = reinterpret_cast<LookupFn>(base + lookup);
                MODULEINFO info {};
                GetModuleInformation(GetCurrentProcess(), mgs4e::game::Module(), &info, sizeof(info));
                g_ImageStart = base;
                g_ImageEnd = base + info.SizeOfImage;
                g_LookInput = safetyhook::create_mid(base + look, [](SafetyHookContext&) { if (MouseAim::bLateSample) { SampleLate(); } });
            }
        }
        spdlog::info("PW mouse: late sample {}: look input +{:X} {}, lookup +{:X}, accumulators +{:X} / +{:X}, manager at +{:X}.",
            bLateSample ? "ON" : "off", look, g_LookInput ? "hooked" : "FAILED", lookup,
            g_AccumX ? reinterpret_cast<uintptr_t>(g_AccumX) - base : 0, g_AccumY ? reinterpret_cast<uintptr_t>(g_AccumY) - base : 0,
            g_Manager ? reinterpret_cast<uintptr_t>(g_Manager) - base : 0);
    }

    if (bFractionCarry || bTelemetry)
    {
        const uintptr_t f = mgspwe::sites::Resolve(kTruncateFreeSig);
        const uintptr_t a = mgspwe::sites::Resolve(kTruncateASig);
        const uintptr_t b = mgspwe::sites::Resolve(kTruncateBSig);
        if (f)
        {
            g_TruncateFree = safetyhook::create_mid(base + f, [](SafetyHookContext& ctx)
            {
                const uint8_t* camera = reinterpret_cast<const uint8_t*>(ctx.rbx);
                OnTruncate(3, camera[kFreeCameraIsMouse] != 0, 0, Word(camera + kFreeCameraYaw), nullptr, ctx.xmm6.f32[0]);
            });
        }
        if (a)
        {
            g_TruncateA = safetyhook::create_mid(base + a, [](SafetyHookContext& ctx)
            {
                const uint8_t* camera = reinterpret_cast<const uint8_t*>(ctx.rbx);
                OnTruncate(1, reinterpret_cast<const uint8_t*>(ctx.rdi)[kPlayerIsMouse] != 0, Word(camera + kCameraPitch), Word(camera + kCameraYaw), &ctx.xmm6.f32[0], ctx.xmm3.f32[0]);
            });
        }
        if (b)
        {
            // The yaw turn is read from camera + 0xBC by the hooked instruction itself, so the whole
            // number goes back there. The game only looks at that field's sign afterwards, and
            // recomputes it on the next mouse frame.
            g_TruncateB = safetyhook::create_mid(base + b, [](SafetyHookContext& ctx)
            {
                uint8_t* camera = reinterpret_cast<uint8_t*>(ctx.rbx);
                float yaw = 0;
                std::memcpy(&yaw, camera + kCameraVelocityYaw, 4);
                const float before = yaw;
                OnTruncate(2, reinterpret_cast<const uint8_t*>(ctx.rdi)[kPlayerIsMouse] != 0, Word(camera + kCameraPitch), Word(camera + kCameraYaw), &ctx.xmm6.f32[0], yaw);
                if (yaw != before) { std::memcpy(camera + kCameraVelocityYaw, &yaw, 4); }
            });
        }
        spdlog::info("PW mouse: fraction carry {}, telemetry {}: free camera yaw +{:X} {}, camera A +{:X} {}, camera B +{:X} {}.",
            bFractionCarry ? "ON" : "off", bTelemetry ? "on" : "off", f, g_TruncateFree ? "hooked" : "FAILED", a, g_TruncateA ? "hooked" : "FAILED", b, g_TruncateB ? "hooked" : "FAILED");
    }
}
