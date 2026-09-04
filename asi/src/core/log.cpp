#include "pch.hpp"
#include "log.hpp"

#include "game.hpp"
#include "version.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/null_sink.h>

namespace pfc::log
{
    namespace
    {
        bool g_Verbose = false;
        std::shared_ptr<spdlog::logger> g_Logger;
    }

    void Initialize()
    {
        try
        {
            const std::filesystem::path folder = pfc::game::Root() / "logs";
            std::filesystem::create_directories(folder);
            const std::filesystem::path file = folder / (std::string(PFC_NAME) + "_Game.log");
            g_Logger = spdlog::basic_logger_mt("pfc", file.string(), /*truncate*/ true);
        }
        catch (const std::exception&)
        {
            g_Logger = spdlog::null_logger_mt("pfc");
        }

        g_Logger->set_pattern("[%H:%M:%S.%e] [%l] %v");
        g_Logger->set_level(spdlog::level::trace);
        g_Logger->flush_on(spdlog::level::info);
        spdlog::set_default_logger(g_Logger);

        spdlog::info("{} {} loaded into {}.", PFC_DISPLAY_NAME, PFC_VERSION_STRING, pfc::game::ExePath().filename().string());
        spdlog::info("Game root: {}", pfc::game::Root().string());
    }

    bool Verbose()
    {
        return g_Verbose;
    }

    void SetVerbose(bool verbose)
    {
        g_Verbose = verbose;
    }

    void Shutdown()
    {
        if (g_Logger)
        {
            g_Logger->flush();
        }
        spdlog::shutdown();
    }
}
