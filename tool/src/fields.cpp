#include "pch.hpp"
#include "fields.hpp"

#include "settings_keys.hpp"
#include "version.hpp"

namespace mgs4e::tool
{
    namespace
    {
        namespace K = mgs4e::keys;

        // --- Help text -------------------------------------------------------------------

        constexpr const char* kHelp_DirectXVersion =
            "Which renderer the game starts with, regardless of what its own options screen says.\n"
            "\n"
            "The game keeps this choice in its Steam-synced saved settings and asks for a restart when it changes. Set here, this value wins at every start and the options screen shows it; change it there and it will not stick while this is set. The game saves the running renderer as its own choice when it exits, so after clearing this it stays on whichever ran last.\n"
            "\n"
            "Leave on \"Use Game Setting\" to keep the game's own choice.";

        constexpr const char* kHelp_WindowMode =
            "How the game's window is presented, regardless of its own options.\n"
            "\n"
            "Fullscreen is the game's exclusive mode, Borderless a frameless window the size of the display, Windowed a normal window at the resolution chosen in-game (or the Window Resolution above).\n"
            "\n"
            "Leave on \"Use Game Setting\" to keep the game's own choice.";

        constexpr const char* kHelp_ReplacedShadersDx12 =
            "Applies pixel-shader replacements when the game runs on DirectX 12.\n"
            "\n"
            "Mods built with 3Dmigoto (FusionFix's shadow shaders, for one) ship a ReplacedShadersPS folder and swap its shaders in through a DirectX 11 hook, so on DirectX 12 they do nothing. The game's DirectX 12 shaders are a separate build of the same code, so with the map this mod ships each replacement is ported to its DirectX 12 counterpart when the game builds the pipeline for it. Nothing happens when no such folder is installed.";

        constexpr const char* kHelp_WindowAspectRatio =
            "Overrides the resolution the game runs its window at.\n"
            "\n"
            "Choose an aspect ratio here, then pick a resolution from the list that appears. 16:10 is the slightly taller widescreen shape; 21:9 covers ultrawide monitors and 32:9 super ultrawide ones.\n"
            "\n"
            "This is the output resolution only. Internal Resolution Scale still applies on top of it, so you can run a smaller window while still rendering at a higher internal resolution.\n"
            "\n"
            "Leave on \"Use Game Setting\" to keep whatever the game's own launcher selected.";

        constexpr const char* kHelp_WindowResolution16x9 =
            "The widescreen resolution the game's window runs at.\n"
            "\n"
            "Only used when Window Aspect Ratio is set to 16:9.";

        constexpr const char* kHelp_WindowResolution16x10 =
            "The 16:10 resolution the game's window runs at.\n"
            "\n"
            "Only used when Window Aspect Ratio is set to 16:10.\n"
            "\n"
            "The interface is authored for 16:9; on a 16:10 panel it is a little taller than the picture, and the Ultrawide HUD setting (Centered or Expanded HUD) corrects that the same way it does on 4:3, with thin bars instead of thick ones.";

        constexpr const char* kHelp_WindowResolution21x9 =
            "The ultrawide resolution the game's window runs at.\n"
            "\n"
            "Only used when Window Aspect Ratio is set to 21:9.\n"
            "\n"
            "Pick your monitor's native resolution. 2560x1080 and 3840x1620 are exactly 21:9; 3440x1440 and 3840x1600 are the slightly wider shape most ultrawide panels actually use.";

        constexpr const char* kHelp_WindowResolution32x9 =
            "The super ultrawide resolution the game's window runs at.\n"
            "\n"
            "Only used when Window Aspect Ratio is set to 32:9.\n"
            "\n"
            "Pick your monitor's native resolution. Two 16:9 panels side by side: 3840x1080 is two 1080p screens, 5120x1440 two 1440p, 7680x2160 two 4K.";

        constexpr const char* kHelp_WindowResolution4x3 =
            "The 4:3 resolution the game's window runs at.\n"
            "\n"
            "Only used when Window Aspect Ratio is set to 4:3.\n"
            "\n"
            "The interface is authored for 16:9 and the game squashes it to fit; the Ultrawide HUD setting (Centered or Expanded HUD) corrects that here as it does on an ultrawide.";

        constexpr const char* kHelp_UltrawideHud =
            "How the HUD is laid out when the game runs at a shape other than 16:9 - wider (ultrawide) or taller (4:3).\n"
            "\n"
            "The game draws its whole interface in a 16:9 space and stretches it to fill the window, so on an ultrawide everything comes out wider than it should be, and on 4:3 narrower.\n"
            "\n"
            "Stretched HUD - the game's own behaviour. Nothing is changed.\n"
            "\n"
            "Centered HUD - every widget keeps its proper shape and the whole interface sits centred in a 16:9 area, with the corner widgets pulled in from the screen edges.\n"
            "\n"
            "Expanded HUD - proper shapes as above, and the life bar, camo meter, weapon and item panels move out to the real edges of the screen: the sides on an ultrawide, the top and bottom on 4:3. The wheels and menus stay centred, and the wheel backdrops span the full window.\n"
            "\n"
            "Works from the real window size, so it also applies when Window Aspect Ratio is on \"Use Game Setting\" and your display is ultrawide or 4:3. Does nothing at all at 16:9.";

        constexpr const char* kHelp_SolidEyeOverlayFix =
            "Keeps the labels that follow things in the world on the things they mark: item pickup names, the Solid Eye's enemy and friendly stat blocks, and its targeting icons.\n"
            "\n"
            "Those labels are placed in the game's 16:9 space and then squeezed toward the centre along with the rest of a Centered or Expanded HUD, which slides them off their targets. This moves each one back onto the object it belongs to.\n"
            "\n"
            "Only does anything with Centered HUD or Expanded HUD; a Stretched HUD is left alone.";

        constexpr const char* kHelp_MenuMasking =
            "Covers the part of the screen outside the 16:9 area with black while a menu is up: the title and main menu, the loading screens up to the moment you press a button on a loaded save, the pause menu and the codec.\n"
            "\n"
            "Those screens are drawn for 16:9 and the game shows the world past their edges on an ultrawide (the sides) or 4:3 (the top and bottom). Masking presents them the way they were authored; gameplay is never masked.\n"
            "\n"
            "Needs a Centered HUD or Expanded HUD, since the menus have to be drawn in the 16:9 area first. Does nothing at all at 16:9.";

        constexpr const char* kHelp_CutsceneMasking =
            "Covers the part of the screen outside the 16:9 area with black during the game's in-engine cutscenes, so they are framed the way they were authored.\n"
            "\n"
            "Works from the cutscene camera itself, so it follows every cutscene and every camera cut without a list of scenes. The mask drops the moment the cutscene ends and gameplay resumes.\n"
            "\n"
            "On an ultrawide, turn on Cutscene FOV Compensation with this - without it the cutscene camera crops the top and bottom of the authored picture and the mask then covers the sides too. Does nothing at all at 16:9.";

        constexpr const char* kHelp_CutsceneFovCompensation =
            "Zooms the in-engine cutscene camera out on an ultrawide so the whole authored 16:9 picture fits in the window.\n"
            "\n"
            "The game's cutscene camera keeps the 16:9 width on a wider window and cuts the top and bottom off the picture. This widens the camera by the same amount, restoring the top and bottom; what lies past the sides of the authored picture becomes visible.\n"
            "\n"
            "With Cutscene Masking, the mask covers that extra width and the cutscene shows as its 16:9 picture centred in the window. Without it, the cutscene is simply shown wider than authored.\n"
            "\n"
            "Only applies to windows wider than 16:9; 4:3 already shows the full picture. Does nothing at all at 16:9.";

        constexpr const char* kHelp_FovAdjustment =
            "Widens or narrows the gameplay camera's field of view. The view width relative to the game's own: 100 leaves it as it is, 120 shows 20% more of the world on each axis, 80 shows less.\n"
            "\n"
            "Applies to the gameplay camera only - cutscenes keep their authored framing (see Cutscene FOV Compensation for those). Works at any aspect ratio, including 16:9.\n"
            "\n"
            "On an ultrawide the game crops the top and bottom of the 16:9 view rather than showing more to the sides; 133 on a 21:9 window restores the vertical view of 16:9.";

        constexpr const char* kHelp_InternalResolutionScale =
            "Renders the game internally at a higher resolution than your display, then scales it down on output (supersampling).\n"
            "\n"
            "Your window and output resolution are left alone, so this works even if your monitor cannot go above its native resolution.\n"
            "\n"
            "200 renders at double width and height, i.e. four times the pixels. The cost grows with the square of the setting, so 400 is sixteen times the pixels of 100. This gets very demanding very quickly.\n"
            "\n"
            "The internal buffer is capped at 8192 wide whatever you choose here, so the highest useful setting depends on your window: about 400 at 1080p, 200 at 4K. Past that the value is ignored.\n"
            "\n"
            "Set to 100 to leave the game's own resolution untouched.";

        constexpr const char* kHelp_ShadowResolutionScale =
            "Increases the resolution of the game's shadow maps beyond its highest Shadow Quality setting.\n"
            "\n"
            "200 doubles the shadow map in each dimension, which sharpens shadow edges considerably but uses four times the shadow memory.\n"
            "\n"
            "This is applied on top of the game's own Shadow Quality option, so leave that at its highest setting for the best result.\n"
            "\n"
            "Set to 100 to leave the game's shadow resolution untouched.";

        constexpr const char* kHelp_AnisotropicFiltering =
            "Maximum anisotropy used when sampling textures viewed at a steep angle - floors, walls and terrain seen edge-on.\n"
            "\n"
            "The game's highest texture setting asks for 8x. 16x is the hardware maximum and costs very little on a modern GPU.\n"
            "\n"
            "Only affects textures the game already filters anisotropically, so it sharpens what is there rather than changing the intended look.\n"
            "\n"
            "Set to 0 to leave the game's own filtering untouched.";

        constexpr const char* kHelp_Fxaa =
            "Post-process anti-aliasing.\n"
            "\n"
            "FXAA smooths jagged edges cheaply, but it works on the finished image and softens texture detail along with the edges.\n"
            "\n"
            "If you are using Internal Resolution Scale, the supersampling is already removing those jagged edges with far more information, so FXAA mostly just blurs the result.\n"
            "\n"
            "The game has its own FXAA option in its display menu; this overrides it. Either way the change takes effect when the game restarts.";

        constexpr const char* kHelp_FxaaQuality =
            "How thoroughly FXAA filters the image, when FXAA is on.\n"
            "\n"
            "Slow does the most work and looks the best; Fast does the least. The game ships the middle setting and does not expose this anywhere.\n"
            "\n"
            "Costs very little on a modern GPU. Has no effect when FXAA is off.";

        constexpr const char* kHelp_DebugLogging =
            "Writes extra detail to logs\\" MGS4E_NAME "_Game.log in the game's install folder - every hook it installs, every pattern scan and the values it reads from this settings file. Costs nothing in-game.\n"
            "\n"
            "How to capture a log for a bug report:\n"
            "  1. Tick this box and save.\n"
            "  2. Launch the game from Steam and do whatever goes wrong. If it crashes or hangs, that is the moment to stop.\n"
            "  3. Quit the game (or end it in Task Manager if it hung).\n"
            "  4. Open the game's install folder (Steam: right-click the game > Manage > Browse local files) and go into the \"logs\" folder.\n"
            "  5. Attach " MGS4E_NAME "_Game.log to your report.\n"
            "\n"
            "Reporting a HUD element in the wrong place on an ultrawide screen: with this on and a Centered or Expanded HUD, press F11 in-game while the element is on screen. A record of every HUD element being drawn at that moment is written into the log. Take a screenshot at the same time (Steam's F12) and attach both.\n"
            "\n"
            "The log is rewritten on every launch, so copy it before starting the game again.";

        // --- The table -------------------------------------------------------------------

        std::vector<Page> BuildPages()
        {
            using F = Field;
            const char* G = K::Graphics;

            std::vector<Page> pages;

            pages.push_back({ "Graphics", {
                { "Window", {
                    F::Choice(G, K::WindowAspectRatio, kHelp_WindowAspectRatio, K::WindowAspectRatio_Off,
                        { K::WindowAspectRatio_Off, K::WindowAspectRatio_16_9, K::WindowAspectRatio_16_10, K::WindowAspectRatio_21_9, K::WindowAspectRatio_32_9, K::WindowAspectRatio_4_3 }),

                    // One resolution list per shape, shown only while its shape is selected.
                    F::Choice(G, K::WindowResolution16x9, kHelp_WindowResolution16x9, "1920x1080",
                        { "1280x720", "1366x768", "1600x900", "1920x1080", "2560x1440", "2880x1620", "3200x1800", "3840x2160", "5120x2880", "7680x4320" })
                        .ShownWhen(K::WindowAspectRatio, { K::WindowAspectRatio_16_9 }),

                    F::Choice(G, K::WindowResolution16x10, kHelp_WindowResolution16x10, "1920x1200",
                        { "1280x800", "1440x900", "1680x1050", "1920x1200", "2560x1600", "2880x1800", "3840x2400" })
                        .ShownWhen(K::WindowAspectRatio, { K::WindowAspectRatio_16_10 }),

                    // "21:9" is the marketing name; 3440x1440 and 3840x1600 are really 43:18 and
                    // 12:5, so the list carries the panels people own, not only exact 64:27 shapes.
                    F::Choice(G, K::WindowResolution21x9, kHelp_WindowResolution21x9, "3440x1440",
                        { "2560x1080", "2560x1088", "3440x1440", "3440x1600", "3840x1600", "3840x1620", "5120x2160" })
                        .ShownWhen(K::WindowAspectRatio, { K::WindowAspectRatio_21_9 }),

                    F::Choice(G, K::WindowResolution32x9, kHelp_WindowResolution32x9, "5120x1440",
                        { "3840x1080", "3840x1200", "5120x1440", "5120x1600", "7680x2160" })
                        .ShownWhen(K::WindowAspectRatio, { K::WindowAspectRatio_32_9 }),

                    F::Choice(G, K::WindowResolution4x3, kHelp_WindowResolution4x3, "1024x768",
                        { "640x480", "800x600", "1024x768", "1152x864", "1280x960", "1400x1050", "1440x1080", "1600x1200", "1920x1440", "2048x1536", "2560x1920", "2880x2160", "3200x2400" })
                        .ShownWhen(K::WindowAspectRatio, { K::WindowAspectRatio_4_3 }),

                    F::Choice(G, K::WindowMode, kHelp_WindowMode, K::WindowMode_Off,
                        { K::WindowMode_Off, K::WindowMode_Fullscreen, K::WindowMode_Borderless, K::WindowMode_Windowed }),

                    F::Choice(G, K::DirectXVersion, kHelp_DirectXVersion, K::DirectXVersion_Off,
                        { K::DirectXVersion_Off, K::DirectXVersion_11, K::DirectXVersion_12 }),
                }},

                { "Ultrawide and 4:3", {
                    // Not tied to the aspect selector: it keys off the real window size, so a
                    // native ultrawide display on "Use Game Setting" needs it too.
                    F::Choice(G, K::UltrawideHud, kHelp_UltrawideHud, K::UltrawideHud_Expanded,
                        { K::UltrawideHud_Stretched, K::UltrawideHud_Centered, K::UltrawideHud_Expanded }),

                    // The world-tracked labels and the menu mask need a corrected layout to work on.
                    F::Bool(G, K::SolidEyeOverlayFix, kHelp_SolidEyeOverlayFix, true)
                        .EnabledWhen(K::UltrawideHud, { K::UltrawideHud_Centered, K::UltrawideHud_Expanded }),
                    F::Bool(G, K::MenuMasking, kHelp_MenuMasking, true)
                        .EnabledWhen(K::UltrawideHud, { K::UltrawideHud_Centered, K::UltrawideHud_Expanded }),

                    // The cutscene pair and the FOV key off the camera, not the HUD layout.
                    F::Bool(G, K::CutsceneMasking, kHelp_CutsceneMasking, true),
                    F::Bool(G, K::CutsceneFovCompensation, kHelp_CutsceneFovCompensation, true),
                    F::Int(G, K::FovAdjustment, kHelp_FovAdjustment, 100, 50, 200),
                }},

                { "Rendering", {
                    F::Int(G, K::InternalResolutionScale, kHelp_InternalResolutionScale, 100, 100, 400),
                    F::Int(G, K::ShadowResolutionScale, kHelp_ShadowResolutionScale, 100, 100, 400),
                    F::Bool(G, K::ReplacedShadersDx12, kHelp_ReplacedShadersDx12, true),
                    F::Int(G, K::AnisotropicFiltering, kHelp_AnisotropicFiltering, 0, 0, 16),
                }},

                { "Anti-aliasing", {
                    F::Bool(G, K::Fxaa, kHelp_Fxaa, true),
                    F::Choice(G, K::FxaaQuality, kHelp_FxaaQuality, K::FxaaQuality_Slow,
                        { K::FxaaQuality_Slow, K::FxaaQuality_Medium, K::FxaaQuality_Fast })
                        .EnabledWhen(K::Fxaa, { "true" }),
                }},
            }});

            pages.push_back({ "Troubleshooting", {
                { "Logging", {
                    F::Bool(K::Debugging, K::DebugLogging, kHelp_DebugLogging, false),
                }},
            }});

            return pages;
        }
    }

    // --- Field ---------------------------------------------------------------------------

    std::string Field::DefaultText() const
    {
        switch (type)
        {
        case Type::Bool:   return defaultBool ? "true" : "false";
        case Type::Int:    return std::to_string(defaultInt);
        case Type::Choice: return defaultChoice;
        }
        return {};
    }

    bool Field::Accepts(const std::string& text) const
    {
        switch (type)
        {
        case Type::Bool:
        {
            std::string lower(text);
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return lower == "true" || lower == "false" || lower == "1" || lower == "0"
                || lower == "yes" || lower == "no" || lower == "on" || lower == "off";
        }
        case Type::Int:
        {
            if (text.empty())
            {
                return false;
            }
            size_t i = (text[0] == '-' || text[0] == '+') ? 1 : 0;
            if (i == text.size())
            {
                return false;
            }
            for (; i < text.size(); ++i)
            {
                if (!std::isdigit(static_cast<unsigned char>(text[i])))
                {
                    return false;
                }
            }
            const int v = std::stoi(text);
            return v >= min && v <= max;
        }
        case Type::Choice:
            return std::find_if(choices.begin(), choices.end(), [&](const char* c) { return text == c; }) != choices.end();
        }
        return false;
    }

    Field Field::Bool(const char* section, const char* key, const char* help, bool def)
    {
        Field f;
        f.section = section; f.key = key; f.help = help;
        f.type = Type::Bool;
        f.defaultBool = def;
        return f;
    }

    Field Field::Int(const char* section, const char* key, const char* help, int def, int min, int max)
    {
        Field f;
        f.section = section; f.key = key; f.help = help;
        f.type = Type::Int;
        f.defaultInt = def; f.min = min; f.max = max;
        return f;
    }

    Field Field::Choice(const char* section, const char* key, const char* help, const char* def, std::vector<const char*> choices)
    {
        Field f;
        f.section = section; f.key = key; f.help = help;
        f.type = Type::Choice;
        f.defaultChoice = def;
        f.choices = std::move(choices);
        return f;
    }

    Field& Field::EnabledWhen(const char* parent, std::vector<const char*> values)
    {
        parentKey = parent;
        parentValues = std::move(values);
        parentShows = false;
        return *this;
    }

    Field& Field::ShownWhen(const char* parent, std::vector<const char*> values)
    {
        parentKey = parent;
        parentValues = std::move(values);
        parentShows = true;
        return *this;
    }

    // --- Lookups -------------------------------------------------------------------------

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
