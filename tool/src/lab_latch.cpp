#include "pch.hpp"
#include "lab_latch.hpp"

#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace mgs4e::tool::lablatch
{
    namespace
    {
        // The same literal the plugin logs on a lab boot and the packager refuses a build on. It is
        // a string constant in a Lab build whether or not lab mode is ever entered, which is what
        // makes it usable as a fingerprint of the file on disk.
        constexpr const char* kLabLiteral = "LAB MODE: research instrumentation";

        bool FileContains(const std::filesystem::path& file, const char* needle)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in) { return false; }
            const std::string n(needle);
            std::vector<char> buf(1u << 20);
            std::string carry;   // the tail of the previous chunk, so a match across a boundary is seen
            while (in)
            {
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                const std::streamsize got = in.gcount();
                if (got <= 0) { break; }
                std::string chunk = carry + std::string(buf.data(), static_cast<size_t>(got));
                if (chunk.find(n) != std::string::npos) { return true; }
                carry = chunk.size() >= n.size() ? chunk.substr(chunk.size() - n.size() + 1) : chunk;
            }
            return false;
        }
    }

    std::filesystem::path LatchFile(const Game& game, const std::filesystem::path& gameRoot)
    {
        return gameRoot / (std::string(game.name) + ".lab.latched");
    }

    std::filesystem::path LabSettingsFile(const Game& game, const std::filesystem::path& gameRoot)
    {
        return gameRoot / (std::string(game.name) + ".lab.settings");
    }

    State Inspect(const Game& game, const std::filesystem::path& gameRoot)
    {
        State s;
        std::error_code ec;
        s.plugin = gameRoot / game.exeDir / L"scripts" / (std::string(game.name) + ".asi");
        s.labPluginDeployed = std::filesystem::exists(s.plugin, ec) && FileContains(s.plugin, kLabLiteral);
        s.labSettingsExist = std::filesystem::exists(LabSettingsFile(game, gameRoot), ec);
        s.latched = std::filesystem::exists(LatchFile(game, gameRoot), ec);
        return s;
    }

    std::string Latch(const Game& game, const std::filesystem::path& gameRoot)
    {
        std::error_code ec;
        const std::filesystem::path lab = LabSettingsFile(game, gameRoot);
        const std::filesystem::path shipped = gameRoot / (std::string(game.name) + ".settings");
        if (!std::filesystem::exists(lab, ec))
        {
            if (!std::filesystem::exists(shipped, ec))
            {
                return "There is no " + shipped.filename().string() + " to start the lab settings from. Save your settings once first.";
            }
            std::filesystem::copy_file(shipped, lab, std::filesystem::copy_options::none, ec);
            if (ec) { return "Could not create " + lab.filename().string() + ": " + ec.message(); }
        }
        std::ofstream out(LatchFile(game, gameRoot), std::ios::trunc);
        if (!out) { return "Could not write " + LatchFile(game, gameRoot).filename().string() + "."; }
        out << "Latched lab mode. Delete this file, or press Revert to shipped in the Config Tool, to return to the shipped settings.\n";
        return {};
    }

    std::string Revert(const Game& game, const std::filesystem::path& gameRoot)
    {
        std::error_code ec;
        std::filesystem::remove(LatchFile(game, gameRoot), ec);
        if (ec) { return "Could not remove the latch: " + ec.message(); }
        return {};
    }
}
