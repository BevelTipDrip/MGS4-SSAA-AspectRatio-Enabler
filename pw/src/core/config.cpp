#include "pch.hpp"
#include "config.hpp"

#include "game.hpp"
#include "log.hpp"
#include "ini.hpp"
#include "version.hpp"
#include "pw/settings_keys.hpp"

#if MGS4E_LAB_BUILD
#include "probe.hpp"
#include "draw_census.hpp"
#include "internal_size.hpp"
#endif

namespace mgspwe::config
{
    namespace
    {
#if MGS4E_LAB_BUILD
        bool g_LabMode = false;
#endif
        std::filesystem::path g_File;

        template<typename T>
        void Read(const mgs4e::Ini& ini, const char* section, const char* key, T& out)
        {
            const auto raw = ini.Raw(section, key);
            if (!raw)
            {
                return;
            }
            const auto value = mgs4e::Ini::Convert<T>(*raw);
            if (!value)
            {
                spdlog::warn("[{}] {}: could not read '{}', keeping the default.", section, key, *raw);
                return;
            }
            out = *value;
        }

        template<typename T>
        void Report(const char* section, const char* key, const T& value)
        {
            if (mgs4e::log::Verbose())
            {
                spdlog::info("Config: [{}] {} = {}", section, key, value);
            }
        }

        // Which file to read: the same marker protocol as the MGS4 build (its config.cpp),
        // with this build's names. The harness drops MGSPWEnabler.lab next to
        // MGSPWEnabler.lab.settings right before launch; a fresh marker selects that file and
        // is consumed here, so a lab boot never leaks into the user's next normal launch and
        // the user's own file is never rewritten. A marker older than fifteen minutes is a
        // leftover from a harness that died before the game came up: ignored and cleared.
        std::filesystem::path ChooseFile()
        {
            const std::filesystem::path root = mgs4e::game::Root();
            std::filesystem::path file = root / (std::string(MGSPWE_NAME) + ".settings");

#if MGS4E_LAB_BUILD
            const std::filesystem::path marker = root / (std::string(MGSPWE_NAME) + ".lab");
            std::error_code ec;
            if (std::filesystem::exists(marker, ec))
            {
                const auto written = std::filesystem::last_write_time(marker, ec);
                const bool fresh = !ec
                    && (std::filesystem::file_time_type::clock::now() - written) < std::chrono::minutes(15);
                const std::filesystem::path labFile = root / (std::string(MGSPWE_NAME) + ".lab.settings");
                if (fresh && std::filesystem::exists(labFile, ec))
                {
                    g_LabMode = true;
                    file = labFile;
                }
                else
                {
                    spdlog::warn("Lab marker {} is {} - ignoring it.", marker.string(),
                        fresh ? "present but there is no " + labFile.filename().string() : "stale");
                }
                std::filesystem::remove(marker, ec);
            }
#endif
            return file;
        }

#if MGS4E_LAB_BUILD
        // Research keys, read only from the lab file. Each names a probe in probe.cpp.
        void ReadLabKeys(const mgs4e::Ini& ini)
        {
            using namespace mgspwe::keys;
            Read(ini, Graphics, "Log Loaded Modules", Probe::bLogLoadedModules);
            Read(ini, Graphics, "Log Decrypt Timing", Probe::bLogDecryptTiming);
            Read(ini, Graphics, "Log Command Line", Probe::bLogCommandLine);
            Report(Graphics, "Log Loaded Modules", Probe::bLogLoadedModules);
            Report(Graphics, "Log Decrypt Timing", Probe::bLogDecryptTiming);
            Report(Graphics, "Log Command Line", Probe::bLogCommandLine);
            Read(ini, Graphics, "Draw Census At (s)", DrawCensus::iCensusAtSeconds);
            Read(ini, Graphics, "Draw Census Frames", DrawCensus::iCensusFrames);
            Read(ini, Graphics, "Live Commands", DrawCensus::bLiveCommands);
            Report(Graphics, "Draw Census At (s)", DrawCensus::iCensusAtSeconds);
            Report(Graphics, "Draw Census Frames", DrawCensus::iCensusFrames);
            Report(Graphics, "Live Commands", DrawCensus::bLiveCommands);
            Read(ini, Graphics, "Render Scale", InternalSize::iRenderScale);
            Read(ini, Graphics, "Internal Size Width", InternalSize::iInternalWidth);
            Read(ini, Graphics, "Internal Size Height", InternalSize::iInternalHeight);
            Read(ini, Graphics, "Output Size Width", InternalSize::iOutputWidth);
            Read(ini, Graphics, "Output Size Height", InternalSize::iOutputHeight);
            Report(Graphics, "Render Scale", InternalSize::iRenderScale);
            Report(Graphics, "Internal Size Width", InternalSize::iInternalWidth);
            Report(Graphics, "Internal Size Height", InternalSize::iInternalHeight);
            Report(Graphics, "Output Size Width", InternalSize::iOutputWidth);
            Report(Graphics, "Output Size Height", InternalSize::iOutputHeight);
        }
#endif
    }

    void Load()
    {
        g_File = ChooseFile();

        mgs4e::Ini ini;
        if (!ini.Load(g_File))
        {
            spdlog::warn("No settings file at {} - running with defaults. Run the settings tool from the game folder to create one.",
                g_File.string());
            return;
        }

        spdlog::info("Settings file: {}", g_File.string());
#if MGS4E_LAB_BUILD
        if (g_LabMode)
        {
            spdlog::warn("LAB MODE: research instrumentation enabled for this boot only.");
        }
#endif

        bool verbose = false;
        Read(ini, mgspwe::keys::Debugging, mgspwe::keys::DebugLogging, verbose);
        mgs4e::log::SetVerbose(verbose);
        Report(mgspwe::keys::Debugging, mgspwe::keys::DebugLogging, verbose);

#if MGS4E_LAB_BUILD
        if (g_LabMode)
        {
            ReadLabKeys(ini);
        }
#endif
    }

#if MGS4E_LAB_BUILD
    bool LabMode()
    {
        return g_LabMode;
    }
#endif

    const std::filesystem::path& File()
    {
        return g_File;
    }
}
