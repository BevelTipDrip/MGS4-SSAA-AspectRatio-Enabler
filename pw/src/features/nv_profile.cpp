#include "pch.hpp"
#include "nv_profile.hpp"

#include <format>
#include <fstream>
#include <memory>

namespace
{
    // nvapi.h, pack 8. NvAPI_UnicodeString is 2048 UTF-16 units.
    using NvString = wchar_t[2048];

    struct DrsApplication   // NVDRS_APPLICATION_V4
    {
        uint32_t version;
        uint32_t isPredefined;
        NvString appName;
        NvString userFriendlyName;
        NvString launcher;
        NvString fileInFolder;
        uint32_t flags;
        NvString commandLine;
    };
    static_assert(sizeof(DrsApplication) == 0x500C);

    struct DrsProfile   // NVDRS_PROFILE_V1
    {
        uint32_t version;
        NvString profileName;
        uint32_t gpuSupport;
        uint32_t isPredefined;
        uint32_t numOfApps;
        uint32_t numOfSettings;
    };
    static_assert(sizeof(DrsProfile) == 0x1014);

    union DrsValue
    {
        uint32_t u32;
        struct { uint32_t length; uint8_t data[4096]; } binary;
        NvString text;
    };

    struct DrsSetting   // NVDRS_SETTING_V1
    {
        uint32_t version;
        NvString settingName;
        uint32_t settingId;
        uint32_t settingType;       // 0 = DWORD
        uint32_t settingLocation;   // 0 = stored in this profile, 1 global, 2 base, 3 driver default
        uint32_t isCurrentPredefined;
        uint32_t isPredefinedValid;
        DrsValue predefined;
        DrsValue current;
    };
    static_assert(sizeof(DrsSetting) == 0x3020);

    template <typename T> constexpr uint32_t Version(uint32_t v) { return static_cast<uint32_t>(sizeof(T)) | (v << 16); }

    constexpr uint32_t kIdInitialize = 0x0150E828;
    constexpr uint32_t kIdCreateSession = 0x0694D52E;
    constexpr uint32_t kIdDestroySession = 0xDAD9CFF8;
    constexpr uint32_t kIdLoadSettings = 0x375DBD6B;
    constexpr uint32_t kIdSaveSettings = 0xFCBC7E14;
    constexpr uint32_t kIdFindApplicationByName = 0xEEE566B2;
    constexpr uint32_t kIdFindProfileByName = 0x7E4A9A0B;
    constexpr uint32_t kIdCreateProfile = 0xCC176068;
    constexpr uint32_t kIdDeleteProfile = 0x17093206;
    constexpr uint32_t kIdGetProfileInfo = 0x61CD6FD6;
    constexpr uint32_t kIdCreateApplication = 0x4347A9DE;
    constexpr uint32_t kIdGetSetting = 0x73BF8338;
    constexpr uint32_t kIdSetSetting = 0x577DD202;
    constexpr uint32_t kIdDeleteProfileSetting = 0xE4A26362;

    // NvApiDriverSettings.h
    constexpr uint32_t kVsyncModeId = 0x00A879CF;   // "Vertical sync"
    constexpr uint32_t kVsyncFast = 0x18888888;     // VSYNCMODE_VIRTUAL: "Fast"

    constexpr const wchar_t* kOurProfileName = L"METAL GEAR SOLID PEACE WALKER (MGSPWEnabler)";

    using Handle = void*;
    using CreateSessionFn = int (*)(Handle*);
    using SessionFn = int (*)(Handle);
    using FindApplicationFn = int (*)(Handle, const wchar_t*, Handle*, DrsApplication*);
    using FindProfileFn = int (*)(Handle, const wchar_t*, Handle*);
    using CreateProfileFn = int (*)(Handle, DrsProfile*, Handle*);
    using ProfileFn = int (*)(Handle, Handle);
    using GetProfileInfoFn = int (*)(Handle, Handle, DrsProfile*);
    using CreateApplicationFn = int (*)(Handle, Handle, DrsApplication*);
    using SettingFn = int (*)(Handle, Handle, DrsSetting*);
    using GetSettingFn = int (*)(Handle, Handle, uint32_t, DrsSetting*);
    using DeleteSettingFn = int (*)(Handle, Handle, uint32_t);

    struct Api
    {
        CreateSessionFn createSession;
        SessionFn destroySession, loadSettings, saveSettings;
        FindApplicationFn findApplication;
        FindProfileFn findProfile;
        CreateProfileFn createProfile;
        ProfileFn deleteProfile;
        GetProfileInfoFn getProfileInfo;
        CreateApplicationFn createApplication;
        GetSettingFn getSetting;
        SettingFn setSetting;
        DeleteSettingFn deleteSetting;

        bool Load()
        {
            using NvProfile::Interface;
            createSession = reinterpret_cast<CreateSessionFn>(Interface(kIdCreateSession));
            destroySession = reinterpret_cast<SessionFn>(Interface(kIdDestroySession));
            loadSettings = reinterpret_cast<SessionFn>(Interface(kIdLoadSettings));
            saveSettings = reinterpret_cast<SessionFn>(Interface(kIdSaveSettings));
            findApplication = reinterpret_cast<FindApplicationFn>(Interface(kIdFindApplicationByName));
            findProfile = reinterpret_cast<FindProfileFn>(Interface(kIdFindProfileByName));
            createProfile = reinterpret_cast<CreateProfileFn>(Interface(kIdCreateProfile));
            deleteProfile = reinterpret_cast<ProfileFn>(Interface(kIdDeleteProfile));
            getProfileInfo = reinterpret_cast<GetProfileInfoFn>(Interface(kIdGetProfileInfo));
            createApplication = reinterpret_cast<CreateApplicationFn>(Interface(kIdCreateApplication));
            getSetting = reinterpret_cast<GetSettingFn>(Interface(kIdGetSetting));
            setSetting = reinterpret_cast<SettingFn>(Interface(kIdSetSetting));
            deleteSetting = reinterpret_cast<DeleteSettingFn>(Interface(kIdDeleteProfileSetting));
            // A translation layer (dxvk-nvapi under Proton) answers nvapi_QueryInterface but has no
            // driver profile store: any missing entry point means "not available here".
            return createSession && destroySession && loadSettings && saveSettings && findApplication && findProfile
                && createProfile && deleteProfile && getProfileInfo && createApplication && getSetting && setSetting && deleteSetting;
        }
    };

    // What the option replaced, kept next to the settings file so that unticking can put it back.
    struct State
    {
        bool hadValue = false;        // the profile held its own Vertical sync value
        uint32_t value = 0;
        bool createdProfile = false;  // the exe had no profile and one was made for it
    };

    bool ReadState(const std::filesystem::path& file, State& state)
    {
        std::ifstream in(file);
        if (!in) { return false; }
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == ';') { continue; }
            const size_t eq = line.find('=');
            if (eq == std::string::npos) { continue; }
            auto trim = [](std::string s)
            {
                const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
                return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
            };
            const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
            if (key == "previous vertical sync")
            {
                state.hadValue = value != "none";
                state.value = state.hadValue ? static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 16)) : 0;
            }
            else if (key == "created profile") { state.createdProfile = value == "true"; }
        }
        return true;
    }

    bool WriteState(const std::filesystem::path& file, const State& state)
    {
        std::ofstream out(file, std::ios::trunc);
        if (!out) { return false; }
        out << "; Written by MGSPWEnabler.asi when \"NVIDIA Fast Sync\" was switched on. It records what that option\n"
               "; replaced in the NVIDIA driver's profile for this game, so that switching it off puts it back.\n"
               "; Deleting this file is harmless: the driver keeps Fast Sync and nothing is put back.\n";
        out << "previous vertical sync = " << (state.hadValue ? std::format("{:#010x}", state.value) : std::string("none")) << "\n";
        out << "created profile = " << (state.createdProfile ? "true" : "false") << "\n";
        return out.good();
    }

    std::string VsyncName(uint32_t value)
    {
        switch (value)
        {
        case 0x60925292: return "Use the 3D application setting";
        case 0x08416747: return "Off";
        case 0x47814940: return "On";
        case 0x32610244: return "Adaptive";
        case 0x71271021: return "Adaptive (half refresh rate)";
        case kVsyncFast: return "Fast";
        default: return std::format("{:#010x}", value);
        }
    }

    // The profile's own Vertical sync value: false when the value in effect is inherited (global,
    // base or driver default) or is the profile's NVIDIA-predefined one rather than a user's.
    bool OwnVsync(const Api& api, Handle session, Handle profile, uint32_t& value, uint32_t& effective, int& code)
    {
        auto setting = std::make_unique<DrsSetting>();
        setting->version = Version<DrsSetting>(1);
        code = api.getSetting(session, profile, kVsyncModeId, setting.get());
        if (code != 0) { effective = 0; return false; }
        effective = setting->current.u32;
        if (setting->settingLocation != 0 || setting->isCurrentPredefined) { return false; }
        value = setting->current.u32;
        return true;
    }

    std::string ProfileName(const Api& api, Handle session, Handle profile)
    {
        DrsProfile info {};
        info.version = Version<DrsProfile>(1);
        if (api.getProfileInfo(session, profile, &info) != 0) { return "?"; }
        std::string name;
        for (const wchar_t* p = info.profileName; *p; ++p) { name += *p < 0x80 ? static_cast<char>(*p) : '?'; }
        return name;
    }

    int SetVsync(const Api& api, Handle session, Handle profile, uint32_t value)
    {
        auto setting = std::make_unique<DrsSetting>();
        setting->version = Version<DrsSetting>(1);
        setting->settingId = kVsyncModeId;
        setting->settingType = 0;
        setting->current.u32 = value;
        return api.setSetting(session, profile, setting.get());
    }
}

void* NvProfile::Interface(uint32_t id)
{
    using QueryInterfaceFn = void* (*)(uint32_t);
    static QueryInterfaceFn query = []() -> QueryInterfaceFn
    {
        const HMODULE nvapi = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!nvapi) { return nullptr; }
        const auto q = reinterpret_cast<QueryInterfaceFn>(GetProcAddress(nvapi, "nvapi_QueryInterface"));
        if (!q) { return nullptr; }
        const auto initialize = reinterpret_cast<int (*)()>(q(kIdInitialize));
        if (!initialize || initialize() != 0) { return nullptr; }
        return q;
    }();
    return query ? query(id) : nullptr;
}

NvProfile::Outcome NvProfile::FastSync(bool wanted, const std::filesystem::path& exe, const std::filesystem::path& stateFile, bool dryRun)
{
    Outcome out;
    State state;
    std::error_code ec;
    const bool haveState = ReadState(stateFile, state);
    if (!wanted && !haveState) { return out; }   // never switched on from here: nothing to put back, nothing loaded

    Api api {};
    if (!api.Load())
    {
        out.lines.push_back(Interface(kIdInitialize)
            ? "the driver has no profile interface here (a translation layer?); nothing done."
            : "no NVIDIA driver on this system; nothing to do.");
        return out;
    }
    out.nvidia = true;

    Handle session = nullptr;
    int r = api.createSession(&session);
    if (r != 0 || !session) { out.lines.push_back(std::format("could not open a driver settings session ({}).", r)); return out; }
    struct Closer { const Api& api; Handle session; ~Closer() { api.destroySession(session); } } closer { api, session };
    r = api.loadSettings(session);
    if (r != 0) { out.lines.push_back(std::format("could not load the driver's settings ({}).", r)); return out; }

    Handle profile = nullptr;
    auto app = std::make_unique<DrsApplication>();
    app->version = Version<DrsApplication>(4);
    const int found = api.findApplication(session, exe.c_str(), &profile, app.get());
    bool haveProfile = found == 0 && profile;

    if (!wanted)
    {
        // Put back what was replaced, but only if the value is still the one written from here.
        bool changed = false;
        if (!haveProfile) { out.lines.push_back(std::format("no driver profile matches this exe any more ({}); nothing to put back.", found)); }
        else
        {
            uint32_t own = 0, effective = 0; int code = 0;
            if (!OwnVsync(api, session, profile, own, effective, code) || own != kVsyncFast)
            {
                out.lines.push_back("Vertical sync in this game's driver profile is no longer Fast (changed elsewhere since); left as it is.");
            }
            else
            {
                r = state.hadValue ? SetVsync(api, session, profile, state.value) : api.deleteSetting(session, profile, kVsyncModeId);
                if (r != 0) { out.lines.push_back(std::format("could not put Vertical sync back ({}); the state file is kept for the next start.", r)); return out; }
                changed = true;
                out.lines.push_back(std::format("Vertical sync in this game's driver profile put back to {}.",
                    state.hadValue ? VsyncName(state.value) : std::string("not set (follows the global setting)")));
                if (state.createdProfile)
                {
                    DrsProfile info {};
                    info.version = Version<DrsProfile>(1);
                    if (api.getProfileInfo(session, profile, &info) == 0 && info.numOfSettings == 0 && info.numOfApps <= 1
                        && wcscmp(info.profileName, kOurProfileName) == 0)
                    {
                        r = api.deleteProfile(session, profile);
                        out.lines.push_back(r == 0 ? "the profile made for it held nothing else and was removed."
                                                   : std::format("the empty profile made for it could not be removed ({}); it is harmless.", r));
                    }
                }
            }
        }
        if (dryRun) { out.lines.push_back("dry run: nothing saved, state file kept."); return out; }
        if (changed)
        {
            r = api.saveSettings(session);
            if (r != 0) { out.lines.push_back(std::format("could not save the driver's settings ({}); the state file is kept for the next start.", r)); return out; }
            out.changed = true;
        }
        std::filesystem::remove(stateFile, ec);
        return out;
    }

    // Switched on.
    State replaced;
    if (haveProfile)
    {
        uint32_t own = 0, effective = 0; int code = 0;
        const bool hasOwn = OwnVsync(api, session, profile, own, effective, code);
        if (hasOwn && own == kVsyncFast)
        {
            out.lines.push_back(haveState ? "Vertical sync is Fast in this game's driver profile (set from here on an earlier start)."
                                          : "Vertical sync is already Fast in this game's driver profile (not set from here, so switching this off will leave it).");
            return out;
        }
        replaced.hadValue = hasOwn;
        replaced.value = own;
        replaced.createdProfile = haveState && state.createdProfile;
        out.lines.push_back(std::format("this game's driver profile \"{}\" has {} for Vertical sync{}.",
            ProfileName(api, session, profile),
            hasOwn ? VsyncName(own) : std::string("no value of its own"),
            code == 0 && !hasOwn ? std::format(" (in effect: {})", VsyncName(effective)) : std::string()));
    }
    else
    {
        DrsProfile info {};
        info.version = Version<DrsProfile>(1);
        wcscpy_s(info.profileName, kOurProfileName);
        r = api.createProfile(session, &info, &profile);
        if (r != 0 || !profile)
        {
            // The name is taken: a profile made from here for another copy of the game. Add this exe to it.
            const int created = r;
            r = api.findProfile(session, kOurProfileName, &profile);
            if (r != 0 || !profile) { out.lines.push_back(std::format("no driver profile matches this exe ({}) and one could not be made ({}).", found, created)); return out; }
        }
        else { replaced.createdProfile = true; }
        auto entry = std::make_unique<DrsApplication>();
        entry->version = Version<DrsApplication>(4);
        wcsncpy_s(entry->appName, exe.c_str(), _TRUNCATE);
        wcscpy_s(entry->userFriendlyName, L"METAL GEAR SOLID PEACE WALKER");
        r = api.createApplication(session, profile, entry.get());
        if (r != 0) { out.lines.push_back(std::format("could not add this exe to a driver profile ({}).", r)); return out; }
        out.lines.push_back(std::format("no driver profile matched this exe ({}); {} \"METAL GEAR SOLID PEACE WALKER (MGSPWEnabler)\".",
            found, replaced.createdProfile ? "made" : "added it to"));
        haveProfile = true;
    }

    r = SetVsync(api, session, profile, kVsyncFast);
    if (r != 0) { out.lines.push_back(std::format("could not set Vertical sync to Fast ({}).", r)); return out; }
    if (dryRun)
    {
        uint32_t own = 0, effective = 0; int code = 0;
        const bool hasOwn = OwnVsync(api, session, profile, own, effective, code);
        Handle again = nullptr;
        auto check = std::make_unique<DrsApplication>();
        check->version = Version<DrsApplication>(4);
        const int refound = api.findApplication(session, exe.c_str(), &again, check.get());
        out.lines.push_back(std::format("dry run: inside the session the profile now reads {} (own value {}, code {}); the exe is found again: {} ({}). Nothing saved.",
            VsyncName(effective), hasOwn ? "yes" : "no", code, refound == 0 && again == profile ? "yes, same profile" : "NO", refound));
        return out;
    }
    // State first: if the save then fails the file is removed again, and a crash in between leaves
    // a record of a change that was not made, which the put-back path detects (value is not Fast).
    if (!WriteState(stateFile, replaced)) { out.lines.push_back("could not write the state file next to the settings file, so the driver profile was left alone."); return out; }
    r = api.saveSettings(session);
    if (r != 0)
    {
        if (haveState) { WriteState(stateFile, state); } else { std::filesystem::remove(stateFile, ec); }
        out.lines.push_back(std::format("could not save the driver's settings ({}).", r));
        return out;
    }
    out.changed = true;
    out.lines.push_back(std::format("Vertical sync set to Fast for this game only; it replaced {}. Switching the option off puts that back.",
        replaced.hadValue ? VsyncName(replaced.value) : std::string("nothing (the profile had no value of its own)")));
    return out;
}
