#include "pch.hpp"

#include "stage_automation.hpp"

#include "game.hpp"
#include "mem.hpp"
#include "log.hpp"

#include <cstddef>

namespace
{
    // ======================= The engine's fast-load path =======================
    //
    // Retail MGS4 still contains its whole developer stage loader. What was cut is the code
    // that read mgs4.dev.ecf and drove it - the loader itself, the stage registry, and the
    // request handshake are all still linked in and reachable. So a stage can be loaded from
    // code without a save file, a menu, or a single keystroke of navigation.
    //
    // The handshake, in the order the game performs it:
    //   1. refuse if a transition is already running   (StageRequestBlocked)
    //   2. write the target stage id and set mode 1    (FastLoadStageId / FastLoadMode)
    //   3. set the stage name to the "select" handoff  (SetStageName)
    //   4. raise bits 0 and 4 of the request flags     (StageRequestFlags |= 0x11)
    //   5. commit                                      (FinalizeStageRequest)
    //
    // Steps 2 and 3 look redundant together, but they are not: "select" is the stage the game
    // transitions *to*, and the fast-load id is what that stage reads to decide where to go.

    using DebugUiFrameDelegate = void(__fastcall*)();
    using StageRequestBlockedDelegate = int32_t(__fastcall*)();
    using SetStageNameDelegate = void(__fastcall*)(const char* stageName);
    using FinalizeStageRequestDelegate = void(__fastcall*)();
    using DebugTextDelegate = void(__fastcall*)(uint16_t x, uint16_t y, uint8_t colour, const char* text);

    // The registry is a std::map<std::string, uint32_t> - an MSVC red-black tree. Node layout
    // is the standard _Tree_node followed by the pair, so the key's small-string buffer lands
    // at +0x20 and the mapped id at +0x40.
    struct StageMapNode
    {
        StageMapNode* left;
        StageMapNode* parent;
        StageMapNode* right;
        uint8_t colour;
        uint8_t isNil;
        uint8_t padding[6];
        char nameStorage[16];
        size_t nameSize;
        size_t nameCapacity;
        uint32_t stageId;
    };

    static_assert(offsetof(StageMapNode, nameStorage) == 0x20);
    static_assert(offsetof(StageMapNode, nameSize) == 0x30);
    static_assert(offsetof(StageMapNode, nameCapacity) == 0x38);
    static_assert(offsetof(StageMapNode, stageId) == 0x40);

    struct StageEntry
    {
        std::string name;
        uint32_t id;
    };

    SafetyHookInline DebugUiFrame_hook {};

    StageRequestBlockedDelegate StageRequestBlocked = nullptr;
    SetStageNameDelegate SetStageName = nullptr;
    FinalizeStageRequestDelegate FinalizeStageRequest = nullptr;
    DebugTextDelegate DebugText = nullptr;

    StageMapNode** StageMapHeadStorage = nullptr;
    uint32_t* FastLoadStageId = nullptr;
    uint32_t* FastLoadMode = nullptr;
    uint32_t* StageRequestFlags = nullptr;

    std::vector<StageEntry> g_Stages;

    // The stages the hotkey walks, in order. Either the configured list or the registry
    // filtered down to the playable entries.
    std::vector<StageEntry> g_Playlist;
    size_t g_NextStageIndex = 0;
    bool g_KeyDown = false;
    uint64_t g_FrameCount = 0;
    uint64_t g_AutoStartAt = 0;

    // What the on-screen line says, and until when. Purely so a run can be confirmed without
    // alt-tabbing to a log - which matters here, because alt-tabbing pauses the game.
    std::array<char, 96> g_StatusText {};
    uint64_t g_StatusExpiresAt = 0;

    // ---------------------------------------------------------------------------------------

    // Scans .text only. The whole-image scan in Memory:: walks SizeOfImage, which on this
    // executable is most of a gigabyte; three signatures through it is a visible stall at boot.
    uint8_t* g_TextBegin = nullptr;
    size_t g_TextSize = 0;

    bool ResolveTextSection()
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(pfc::game::Module());
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
            reinterpret_cast<const uint8_t*>(pfc::game::Module()) + dos->e_lfanew);

        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++)
        {
            if (std::memcmp(section->Name, ".text", 5) == 0)
            {
                g_TextBegin = reinterpret_cast<uint8_t*>(pfc::game::Module()) + section->VirtualAddress;
                g_TextSize = section->Misc.VirtualSize;
                return true;
            }
        }
        return false;
    }

    uint8_t* ScanText(const char* signature)
    {
        std::vector<int> bytes;
        for (const char* cursor = signature; *cursor; )
        {
            if (*cursor == ' ')
            {
                cursor++;
            }
            else if (*cursor == '?')
            {
                while (*cursor == '?') cursor++;
                bytes.push_back(-1);
            }
            else
            {
                char* end = nullptr;
                bytes.push_back(static_cast<int>(strtoul(cursor, &end, 16)));
                cursor = end;
            }
        }

        if (bytes.empty() || g_TextSize < bytes.size())
        {
            return nullptr;
        }

        const size_t last = g_TextSize - bytes.size();
        for (size_t i = 0; i <= last; i++)
        {
            bool matched = true;
            for (size_t j = 0; j < bytes.size(); j++)
            {
                if (bytes[j] != -1 && g_TextBegin[i + j] != bytes[j])
                {
                    matched = false;
                    break;
                }
            }
            if (matched)
            {
                return g_TextBegin + i;
            }
        }
        return nullptr;
    }

    uintptr_t Rva(uintptr_t address)
    {
        return address - reinterpret_cast<uintptr_t>(pfc::game::Module());
    }

    uintptr_t Rva(const void* address)
    {
        return Rva(reinterpret_cast<uintptr_t>(address));
    }

    // ---------------------------------------------------------------------------------------

    bool ReadStageName(const StageMapNode* node, std::string& name)
    {
        if (node->nameSize == 0 || node->nameSize >= 16)
        {
            return false;
        }

        // MSVC's small-string buffer holds a pointer instead of characters once the string
        // has ever needed the heap, and capacity is what says which.
        const char* source = node->nameStorage;
        if (node->nameCapacity >= sizeof(node->nameStorage))
        {
            std::memcpy(&source, node->nameStorage, sizeof(source));
        }

        if (!pfc::mem::Readable(source, node->nameSize + 1) || source[node->nameSize] != '\0')
        {
            return false;
        }

        name.assign(source, node->nameSize);
        return true;
    }

    bool CollectStages(StageMapNode* node, StageMapNode* head, size_t& visited,
        std::vector<StageEntry>& entries)
    {
        if (!node || node == head)
        {
            return true;
        }
        if (!pfc::mem::Readable(node, sizeof(*node)))
        {
            return false;
        }
        if (node->isNil != 0)
        {
            return true;
        }
        if (++visited > 2048)
        {
            return false;
        }

        if (!CollectStages(node->left, head, visited, entries))
        {
            return false;
        }

        std::string name;
        // "select" is the handoff stage the request goes through, not a destination.
        if (node->stageId != 0 && ReadStageName(node, name) && name != "select")
        {
            entries.push_back({ std::move(name), node->stageId });
        }

        return CollectStages(node->right, head, visited, entries);
    }

    bool RefreshStages()
    {
        if (!StageMapHeadStorage)
        {
            return false;
        }

        StageMapNode* head = *StageMapHeadStorage;
        if (!pfc::mem::Readable(head, sizeof(*head)) || head->isNil == 0)
        {
            return false;
        }

        std::vector<StageEntry> entries;
        size_t visited = 0;
        if (!CollectStages(head->parent, head, visited, entries) || entries.empty())
        {
            return false;
        }

        g_Stages = std::move(entries);
        return true;
    }

    // "_D" marks the cutscene variant of a stage. Those never hand control to the player, so
    // they can never be in an aiming state and are only noise in a weapon survey.
    bool IsCutsceneStage(const std::string& name)
    {
        const size_t underscore = name.rfind('_');
        return underscore != std::string::npos
            && underscore + 1 < name.size()
            && name[underscore + 1] == 'D';
    }

    // Builds the walk order once the registry exists. A configured list is taken in the order
    // it was written - the point of the list is to control what gets tried and when.
    void BuildPlaylist()
    {
        g_Playlist.clear();
        g_NextStageIndex = 0;

        if (StageAutomation::sStage.empty())
        {
            for (const StageEntry& entry : g_Stages)
            {
                if (!IsCutsceneStage(entry.name))
                {
                    g_Playlist.push_back(entry);
                }
            }
            spdlog::info("MGS4: Stage automation: playlist is the registry minus cutscenes, "
                "{} of {} stage(s).", g_Playlist.size(), g_Stages.size());
            return;
        }

        std::stringstream names(StageAutomation::sStage);
        std::string name;
        while (std::getline(names, name, ','))
        {
            const size_t first = name.find_first_not_of(" \t\"");
            const size_t last = name.find_last_not_of(" \t\"");
            if (first == std::string::npos)
            {
                continue;
            }
            name = name.substr(first, last - first + 1);

            const auto match = std::find_if(g_Stages.begin(), g_Stages.end(),
                [&name](const StageEntry& entry) { return entry.name == name; });

            if (match == g_Stages.end())
            {
                spdlog::warn("MGS4: Stage automation: '{}' is not in the registry; skipped.", name);
            }
            else
            {
                g_Playlist.push_back(*match);
            }
        }

        spdlog::info("MGS4: Stage automation: playlist has {} configured stage(s).",
            g_Playlist.size());
    }

    void SetStatus(const std::string& text)
    {
        std::snprintf(g_StatusText.data(), g_StatusText.size(), "%s", text.c_str());
        g_StatusExpiresAt = GetTickCount64() + 8000;
    }

    // ======================= Driving the rest of the run =======================
    //
    // A stage request alone does not get us to a crosshair. The engine puts up a confirmation
    // prompt first, and only once that is dismissed does the transition actually run.
    //
    // Timing that blind would be fragile - load times vary with the stage and with whatever
    // else the machine is doing. Instead every wait is gated on StageRequestBlocked(), which
    // is the engine's own "a transition is in flight" answer: it goes non-zero when the
    // transition starts and back to zero when the new stage is up. So the sequence presses
    // Enter only until the transition takes, and waits exactly as long as the load needs.

    enum class Phase
    {
        Idle,
        Confirming,   // prompt is up; press Enter until the engine reports a transition
        Loading,      // transition in flight; wait for it to clear
        Settling,     // stage is up; let it finish spawning before touching the controls
        Aiming,       // aim held, diagnostics sampling
        Done,
    };

    Phase g_Phase = Phase::Idle;
    uint64_t g_PhaseDeadline = 0;
    uint64_t g_NextConfirmAt = 0;
    int g_ConfirmPresses = 0;
    std::string g_RunStage;

    constexpr int kMaxConfirmPresses = 12;
    constexpr uint64_t kConfirmIntervalMs = 500;
    constexpr uint64_t kConfirmTimeoutMs = 20000;
    constexpr uint64_t kLoadTimeoutMs = 180000;
    constexpr uint64_t kSettleMs = 4000;

    // Synthesised input reaches the game through its window message queue, so unlike our own
    // GetAsyncKeyState hotkey read it only lands if the game is actually focused. Say so once
    // rather than letting a run fail silently and look like a bad hypothesis.
    bool GameHasFocus()
    {
        DWORD owner = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &owner);
        return owner == GetCurrentProcessId();
    }

    void SendKey(WORD virtualKey)
    {
        INPUT input[2] {};
        input[0].type = INPUT_KEYBOARD;
        input[0].ki.wVk = virtualKey;
        input[1] = input[0];
        input[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, input, sizeof(INPUT));
    }

    void SendAim(bool down)
    {
        INPUT input {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        SendInput(1, &input, sizeof(input));
    }

    void BeginSequence(const std::string& stageName)
    {
        if (!StageAutomation::bAutoSequence)
        {
            return;
        }

        g_RunStage = stageName;
        g_Phase = Phase::Confirming;
        g_ConfirmPresses = 0;
        g_NextConfirmAt = GetTickCount64() + kConfirmIntervalMs;
        g_PhaseDeadline = GetTickCount64() + kConfirmTimeoutMs;

        if (!GameHasFocus())
        {
            spdlog::warn("MGS4: Stage automation: the game is not focused, so synthesised input "
                "will go to another window. Focus the game for an automated run.");
        }
    }

    void FinishRun(const char* outcome)
    {
        spdlog::info("MGS4: Stage automation: run on '{}' finished: {}.", g_RunStage, outcome);
        SetStatus(std::format("run finished: {}", outcome));
        g_Phase = Phase::Done;

        if (StageAutomation::bExitAfterRun)
        {
            spdlog::info("MGS4: Stage automation: exiting as configured.");
            spdlog::default_logger()->flush();
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }

    void AdvanceSequence()
    {
        if (g_Phase == Phase::Idle || g_Phase == Phase::Done)
        {
            return;
        }

        const uint64_t now = GetTickCount64();
        const bool transitionActive = StageRequestBlocked() != 0;

        switch (g_Phase)
        {
        case Phase::Confirming:
            // The prompt sits between the request and the load. Press until the engine says a
            // transition started, then stop - Enter is also the pause button once in gameplay,
            // so pressing past this point would land us in a menu instead of aiming.
            if (transitionActive)
            {
                spdlog::info("MGS4: Stage automation: transition started after {} press(es).",
                    g_ConfirmPresses);
                g_Phase = Phase::Loading;
                g_PhaseDeadline = now + kLoadTimeoutMs;
            }
            else if (now >= g_PhaseDeadline || g_ConfirmPresses >= kMaxConfirmPresses)
            {
                FinishRun("confirmation prompt never led to a transition");
            }
            else if (now >= g_NextConfirmAt)
            {
                g_ConfirmPresses++;
                g_NextConfirmAt = now + kConfirmIntervalMs;
                SendKey(VK_RETURN);
                SetStatus(std::format("confirming '{}' ({})", g_RunStage, g_ConfirmPresses));
            }
            break;

        case Phase::Loading:
            if (!transitionActive)
            {
                spdlog::info("MGS4: Stage automation: '{}' loaded; settling.", g_RunStage);
                g_Phase = Phase::Settling;
                g_PhaseDeadline = now + kSettleMs;
                SetStatus(std::format("'{}' loaded", g_RunStage));
            }
            else if (now >= g_PhaseDeadline)
            {
                FinishRun("load never completed");
            }
            break;

        case Phase::Settling:
            if (now >= g_PhaseDeadline)
            {
                if (!GameHasFocus())
                {
                    FinishRun("game lost focus, so the aim input would not reach it");
                    break;
                }

                spdlog::info("MGS4: Stage automation: holding aim for {}s.",
                    StageAutomation::iAimHoldSeconds);
                SendAim(true);
                g_Phase = Phase::Aiming;
                g_PhaseDeadline = now
                    + static_cast<uint64_t>(std::max(1, StageAutomation::iAimHoldSeconds)) * 1000;
                SetStatus("aiming");
            }
            break;

        case Phase::Aiming:
            if (now >= g_PhaseDeadline)
            {
                SendAim(false);
                FinishRun("aim window elapsed");
            }
            break;

        default:
            break;
        }
    }

    bool RequestStage(const StageEntry& stage)
    {
        if (StageRequestBlocked() != 0)
        {
            spdlog::warn("MGS4: Stage automation: '{}' refused, a transition is already running.",
                stage.name);
            SetStatus(std::format("stage '{}' refused - transition busy", stage.name));
            return false;
        }

        *FastLoadStageId = stage.id;
        *FastLoadMode = 1;
        SetStageName("select");
        *StageRequestFlags |= 0x11;
        FinalizeStageRequest();

        spdlog::info("MGS4: Stage automation: requested '{}' (id 0x{:X}).", stage.name, stage.id);
        SetStatus(std::format("loading '{}' (id 0x{:X})", stage.name, stage.id));
        BeginSequence(stage.name);
        return true;
    }

    // Shared by the hotkey and the auto-start, so both walk the playlist the same way.
    void RequestNextInPlaylist()
    {
        if (g_Stages.empty() && RefreshStages())
        {
            BuildPlaylist();
        }

        if (g_Playlist.empty())
        {
            spdlog::warn("MGS4: Stage automation: nothing to load - the registry has not "
                "populated, or no configured stage name matched it.");
            SetStatus("nothing queued to load");
            return;
        }

        // One press, one stage, wrapping at the end. Walking the list in a single session is
        // what makes surveying candidates cheap - the alternative is a relaunch per stage,
        // which is the cost this whole module exists to remove.
        if (g_NextStageIndex >= g_Playlist.size())
        {
            g_NextStageIndex = 0;
        }
        const StageEntry stage = g_Playlist[g_NextStageIndex];
        spdlog::info("MGS4: Stage automation: playlist entry {} of {}.",
            g_NextStageIndex, g_Playlist.size());
        g_NextStageIndex++;
        RequestStage(stage);
    }

    // Reports every edge of the engine's transition flag, always, independent of the
    // sequencer. The orchestrating script drives the keyboard and can see the screen, but it
    // cannot see this flag - and the flag is the only unambiguous answer to "has the stage
    // actually started loading", which brightness heuristics on a screenshot are not.
    void ReportTransitionEdges()
    {
        static int lastState = -1;

        const int state = StageRequestBlocked() != 0 ? 1 : 0;
        if (state == lastState)
        {
            return;
        }

        // Skip the first observation: it establishes the baseline rather than marking a change.
        if (lastState != -1)
        {
            spdlog::info("MGS4: Stage automation: transition {}.",
                state != 0 ? "STARTED" : "FINISHED");
        }
        lastState = state;
    }

    void Tick()
    {
        g_FrameCount++;
        ReportTransitionEdges();

        // The registry is built during boot, so it does not exist on the first frames. Retry
        // periodically rather than once, and log the contents the moment it appears - that
        // list is the thing we do not have yet.
        if (g_Stages.empty() && (g_FrameCount % 120) == 1)
        {
            if (RefreshStages())
            {
                spdlog::info("MGS4: Stage automation: registry populated with {} stage(s).",
                    g_Stages.size());

                if (StageAutomation::bLogStageRegistry)
                {
                    std::string line;
                    for (size_t i = 0; i < g_Stages.size(); i++)
                    {
                        line += std::format("  [{:3}] {:<16} id 0x{:X}\n",
                            i, g_Stages[i].name, g_Stages[i].id);
                    }
                    spdlog::info("MGS4: Stage automation: stage registry:\n{}", line);
                }

                BuildPlaylist();
                SetStatus(std::format("registry: {} stages, {} queued",
                    g_Stages.size(), g_Playlist.size()));

                // The registry exists well before the title screen finishes, so give the game
                // room to reach a state where it will act on a request rather than firing the
                // instant the map is readable.
                if (StageAutomation::bAutoStartOnBoot)
                {
                    g_AutoStartAt = GetTickCount64() + 8000;
                }
            }
        }

        if (g_AutoStartAt != 0 && GetTickCount64() >= g_AutoStartAt)
        {
            g_AutoStartAt = 0;
            spdlog::info("MGS4: Stage automation: auto-starting the first playlist entry.");
            RequestNextInPlaylist();
        }

        const bool keyDown = (GetAsyncKeyState(StageAutomation::iLoadStageKey) & 0x8000) != 0;
        const bool pressed = keyDown && !g_KeyDown;
        g_KeyDown = keyDown;

        if (pressed)
        {
            RequestNextInPlaylist();
        }

        AdvanceSequence();

        if (DebugText && g_StatusText[0] != '\0' && GetTickCount64() < g_StatusExpiresAt)
        {
            DebugText(0, 5, 0x0f, g_StatusText.data());
        }
    }

    void __fastcall DebugUiFrameHook()
    {
        // Anything that touches game state has to run on the game thread, which this is.
        // Failing loudly here would take the frame down with it, so keep it contained: a
        // fault in the automation should cost the run, not the session.
        if (StageAutomation::bEnabled)
        {
            __try
            {
                Tick();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                StageAutomation::bEnabled = false;
                spdlog::error("MGS4: Stage automation: tick faulted; disabled for this session.");
            }
        }

        DebugUiFrame_hook.call<void>();
    }
}

namespace StageAutomation
{
    void ApplyFixes()
    {
        if (!bEnabled)
        {
            return;
        }

        if (!ResolveTextSection())
        {
            spdlog::error("MGS4: Stage automation: could not locate .text.");
            return;
        }

        // Signatures from cipherxof/MGS4-Debug. Re-derived against this build rather than
        // trusted - each is validated below against the exact opcodes we then decode, so a
        // game update that moves things produces a clean refusal instead of a wild pointer.
        constexpr char kUiFramePattern[] =
            "48 83 EC 28 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 0F B7 90 BA 00 00 00 "
            "0F B7 88 B8 00 00 00 E8 ?? ?? ?? ?? 48 83 C4 28 E9 ?? ?? ?? ??";
        constexpr char kFastLoadStagePattern[] =
            "40 57 41 56 41 57 48 83 EC 20 48 8B F9 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? "
            "FF 15 ?? ?? ?? ?? 4C 8B 7F 18 48 8B C7 4C 8B 77 10 49 83 FF 10 72 ?? "
            "48 8B 07 49 83 FE 06 75 ?? 81 38 73 65 6C 65 75 ??";
        constexpr char kStageRequestHandlerPattern[] =
            "48 83 EC 28 E8 ?? ?? ?? ?? 85 C0 0F 85 ?? ?? ?? ?? 48 89 5C 24 20 "
            "E8 ?? ?? ?? ?? 48 8B D8 80 38 00 0F 84 ?? ?? ?? ?? B1 6E E8 ?? ?? ?? ?? "
            "48 85 C0 74 ?? E8 ?? ?? ?? ?? 48 85 C0 74 ?? E8 ?? ?? ?? ?? 85 C0 75 ??";
        constexpr char kStageNameOverlayPattern[] =
            "48 83 EC 28 80 3D ?? ?? ?? ?? 00 74 ?? 48 89 5C 24 30 48 C7 C3 FF FF FF FF "
            "48 89 74 24 38 48 8B 35 ?? ?? ?? ??";

        uint8_t* uiFrame = ScanText(kUiFramePattern);
        uint8_t* fastLoadStage = ScanText(kFastLoadStagePattern);
        uint8_t* stageRequestHandler = ScanText(kStageRequestHandlerPattern);
        uint8_t* stageNameOverlay = ScanText(kStageNameOverlayPattern);

        if (!uiFrame || !fastLoadStage || !stageRequestHandler)
        {
            spdlog::error("MGS4: Stage automation: signature scan failed "
                "(uiFrame {}, fastLoadStage {}, stageRequestHandler {}).",
                uiFrame != nullptr, fastLoadStage != nullptr, stageRequestHandler != nullptr);
            return;
        }

        // Validate every byte we are about to decode a displacement or a call target out of.
        // Without this a shifted match silently produces a pointer into nothing, and the
        // failure surfaces as a crash several seconds later inside game code.
        const bool valid =
            fastLoadStage[0x85] == 0x4C && fastLoadStage[0x86] == 0x8B && fastLoadStage[0x87] == 0x2D
            && fastLoadStage[0x122] == 0x89 && fastLoadStage[0x123] == 0x05
            && stageRequestHandler[0x04] == 0xE8
            && stageRequestHandler[0xA8] == 0xE8
            && stageRequestHandler[0xAD] == 0x83 && stageRequestHandler[0xAE] == 0x0D
            && stageRequestHandler[0xB3] == 0x01
            && stageRequestHandler[0xD2] == 0xE8;

        if (!valid)
        {
            spdlog::error("MGS4: Stage automation: signature matched but the surrounding "
                "opcodes did not; refusing to resolve pointers from it.");
            return;
        }

        StageMapHeadStorage = reinterpret_cast<StageMapNode**>(
            pfc::mem::RipTarget(reinterpret_cast<uintptr_t>(fastLoadStage + 0x88)));
        FastLoadStageId = reinterpret_cast<uint32_t*>(
            pfc::mem::RipTarget(reinterpret_cast<uintptr_t>(fastLoadStage + 0x124)));
        FastLoadMode = FastLoadStageId + 1;

        // The flags live one dword past the operand of the `cmp dword [rip+d], 1` at +0xAD;
        // that instruction tests the neighbouring field, not the flags themselves.
        StageRequestFlags = reinterpret_cast<uint32_t*>(
            pfc::mem::RipTarget(reinterpret_cast<uintptr_t>(stageRequestHandler + 0xAF)) + 1);

        StageRequestBlocked = reinterpret_cast<StageRequestBlockedDelegate>(
            pfc::mem::CallTarget(stageRequestHandler + 0x04));
        SetStageName = reinterpret_cast<SetStageNameDelegate>(
            pfc::mem::CallTarget(stageRequestHandler + 0xA8));
        FinalizeStageRequest = reinterpret_cast<FinalizeStageRequestDelegate>(
            pfc::mem::CallTarget(stageRequestHandler + 0xD2));

        // Optional - only drives the on-screen status line.
        if (stageNameOverlay && stageNameOverlay[0x77] == 0xE8)
        {
            DebugText = reinterpret_cast<DebugTextDelegate>(
                pfc::mem::CallTarget(stageNameOverlay + 0x77));
        }

        spdlog::info("MGS4: Stage automation: uiFrame +{:X}, fastLoadStage +{:X}, "
            "stageRequestHandler +{:X}.",
            Rva(uiFrame), Rva(fastLoadStage), Rva(stageRequestHandler));
        spdlog::info("MGS4: Stage automation: stageMapHead +{:X}, fastLoadStageId +{:X}, "
            "stageRequestFlags +{:X}.",
            Rva(StageMapHeadStorage), Rva(FastLoadStageId), Rva(StageRequestFlags));
        spdlog::info("MGS4: Stage automation: stageRequestBlocked +{:X}, setStageName +{:X}, "
            "finalizeStageRequest +{:X}, debugText {}.",
            Rva(reinterpret_cast<void*>(StageRequestBlocked)),
            Rva(reinterpret_cast<void*>(SetStageName)),
            Rva(reinterpret_cast<void*>(FinalizeStageRequest)),
            DebugText ? std::format("+{:X}", Rva(reinterpret_cast<void*>(DebugText))) : "unresolved");

        DebugUiFrame_hook = safetyhook::create_inline(uiFrame,
            reinterpret_cast<void*>(DebugUiFrameHook));

        if (!DebugUiFrame_hook)
        {
            spdlog::error("MGS4: Stage automation: failed to hook the per-frame tick.");
            return;
        }

        spdlog::info("MGS4: Stage automation: ready. Press VK 0x{:X} to walk {}.",
            iLoadStageKey,
            sStage.empty() ? std::string("the playable stages in the registry")
                           : std::format("'{}'", sStage));
    }
}
