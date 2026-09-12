#include "pch.hpp"
#include "games.hpp"

#include "pw/settings_keys.hpp"
#include "pw/identity.hpp"

// The Peace Walker settings the tool edits: the research knobs a Lab build of the ASI reads
// (docs/pw/lab.md) and the logging switch. A Release build reads only the logging switch.
namespace mgs4e::tool::pw
{
    namespace
    {
        namespace K = mgspwe::keys;
        using F = Field;

        constexpr const char* kHelp_DebugLogging =
            "Writes more detail to logs\\" MGSPWE_NAME "_Game.log: which hooks were installed and what each setting resolved to.\n"
            "\n"
            "Turn it on before reproducing a problem, then attach the log to your report.";

        constexpr const char* kHelp_RenderScale =
            "The scene renders at this integer multiple of the PSP's 480x272 canvas; the game's own launcher\n"
            "settings are 3 (1440x816) and 4 (1920x1088). 8 is 3840x2176 (4K wide), 12 is 5760x3264, 16 is\n"
            "7680x4352 (8K wide). The game's own final blit downscales into the window; no oversized window.\n"
            "0 leaves the game's value.\n"
            "\n"
            "Needs a Lab build of " MGSPWE_NAME ".asi in mgspw\\scripts; a Release build ignores this tab.";
        constexpr const char* kHelp_OutputSize =
            "The picture the game fits into the window (its output table). Set it to the window size to fill a\n"
            "16:9 window, or to a 16:9 picture that fits the window height on a wider display (the game then\n"
            "pillarboxes it). 0 leaves the game's table.";
        constexpr const char* kHelp_InternalSize =
            "The post-processing chain's target size, when it should differ from the scene. 0 = same as the\n"
            "scene (the only configuration measured to be a win so far).";
        constexpr const char* kHelp_DisableMsaa =
            "Creates the scene targets single-sampled and turns the game's resolve into a copy. About 2 ms at 8K.\n"
            "The game's own resolve shaders are written for 4 samples; check aiming and depth of field with it on.";
        constexpr const char* kHelp_GpuLocal =
            "The game's full-size frame textures carry CPU write access and end up in system memory, so its\n"
            "per-frame copies into them cross PCIe (5 ms each at 8K). This keeps them in video memory. The game\n"
            "was never seen to write them from the CPU; the log reports it if it ever does.";
        constexpr const char* kHelp_BackBuffer =
            "Experiment: the swap chain's buffers at the scene size, stretched onto the window by the compositor\n"
            "instead of the game's blit. Measured to cost about the same as the game's own blit.";
        constexpr const char* kHelp_Census =
            "Draw census: logs every draw of N frames (targets, shaders, textures, callers) to the game log, at\n"
            "the given time after start. 0 = only on command. Use 2 frames: the first is always empty.";
        constexpr const char* kHelp_Live =
            "Polls C:\\mgspf_tools\\pw\\live.txt for commands and F11 for a census plus the timing run.";
        constexpr const char* kHelp_Timing =
            "GPU timestamps around the passes of a census frame, averaged over this many frames (one in four),\n"
            "reported in the game log with the frame rate over the window.";
        constexpr const char* kHelp_DumpShaders =
            "Writes every shader's bytecode to C:\\mgspf_tools\\pw\\shaders for disassembly.";
        constexpr const char* kHelp_LightHooks =
            "Leaves the game's draws unhooked (only the resolve is hooked): the frame rate of a Release-like\n"
            "build with the rendering knobs still applied. No census or timing in this mode.";
        constexpr const char* kHelp_Probe =
            "One-off log lines at start-up: the loaded modules, the code decryption timing, the command line.";

        std::vector<Page> BuildPages()
        {
            const char* G = K::Graphics;
            const char* D = K::Debugging;
            return {
                { "Rendering (Lab)", {
                    { "Internal resolution", {
                        F::Int(G, K::RenderScale, kHelp_RenderScale, 0, 0, 24),
                        F::Bool(G, K::DisableMsaa, kHelp_DisableMsaa, false),
                        F::Bool(G, K::GpuLocalTextures, kHelp_GpuLocal, false),
                    }},
                    { "Sizes (0 = the game's)", {
                        F::Int(G, K::OutputSizeWidth, kHelp_OutputSize, 0, 0, 16384),
                        F::Int(G, K::OutputSizeHeight, kHelp_OutputSize, 0, 0, 16384),
                        F::Int(G, K::InternalSizeWidth, kHelp_InternalSize, 0, 0, 16384),
                        F::Int(G, K::InternalSizeHeight, kHelp_InternalSize, 0, 0, 16384),
                        F::Bool(G, K::BackBufferAtInternalSize, kHelp_BackBuffer, false),
                    }},
                }},
                { "Lab", {
                    { "Draw census", {
                        F::Int(G, K::DrawCensusAtSeconds, kHelp_Census, 0, 0, 3600),
                        F::Int(G, K::DrawCensusFrames, kHelp_Census, 2, 1, 10),
                        F::Bool(G, K::LiveCommands, kHelp_Live, true),
                    }},
                    { "GPU timing", {
                        F::Bool(G, K::TimePasses, kHelp_Timing, true),
                        F::Int(G, K::TimeFrames, kHelp_Timing, 60, 1, 600),
                    }},
                    { "Other", {
                        F::Bool(G, K::DumpShaders, kHelp_DumpShaders, false),
                        F::Bool(G, K::LightHooks, kHelp_LightHooks, false),
                        F::Bool(G, K::LogDecryptTiming, kHelp_Probe, false),
                        F::Bool(G, K::LogCommandLine, kHelp_Probe, false),
                        F::Bool(G, K::LogLoadedModules, kHelp_Probe, false),
                    }},
                }},
                { "Troubleshooting", {
                    { "Logging", {
                        F::Bool(D, K::DebugLogging, kHelp_DebugLogging, false),
                    }},
                }},
            };
        }
    }

    const std::vector<Page>& Pages()
    {
        static const std::vector<Page> pages = BuildPages();
        return pages;
    }

    const std::vector<const Field*>& AllFields()
    {
        static const std::vector<const Field*> all = []
        {
            std::vector<const Field*> v;
            for (const Page& p : Pages())
            {
                for (const Group& g : p.groups)
                {
                    for (const Field& f : g.fields)
                    {
                        v.push_back(&f);
                    }
                }
            }
            return v;
        }();
        return all;
    }

    const Field* FindField(const std::string& section, const std::string& key)
    {
        for (const Field* f : AllFields())
        {
            if (section == f->section && key == f->key)
            {
                return f;
            }
        }
        return nullptr;
    }
}
