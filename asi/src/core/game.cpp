#include "pch.hpp"
#include "game.hpp"

namespace pfc::game
{
    namespace
    {
        HMODULE g_Module = nullptr;
        std::filesystem::path g_ExePath;
        std::filesystem::path g_Root;
        bool g_IsMgs4 = false;
    }

    void Detect()
    {
        g_Module = GetModuleHandleW(nullptr);

        wchar_t buffer[MAX_PATH * 4] {};
        GetModuleFileNameW(g_Module, buffer, static_cast<DWORD>(std::size(buffer)));
        g_ExePath = buffer;
        g_Root = g_ExePath.parent_path().parent_path();

        std::wstring name = g_ExePath.filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), ::towlower);
        g_IsMgs4 = (name == L"mgs4.exe");
    }

    HMODULE Module()
    {
        return g_Module ? g_Module : GetModuleHandleW(nullptr);
    }

    const std::filesystem::path& ExePath()
    {
        return g_ExePath;
    }

    const std::filesystem::path& Root()
    {
        return g_Root;
    }

    bool IsMgs4()
    {
        return g_IsMgs4;
    }
}
