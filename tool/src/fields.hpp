#pragma once

#include <string>
#include <vector>

// The settings the tool edits, in the order they appear on screen.
//
// Every entry names a section and key in MGS4Enabler.settings (the strings come from
// shared/settings_keys.hpp so the ASI reads what the tool writes), its type and default,
// and the text shown as its tooltip. A field may depend on another in the same section:
// it is only usable while the parent holds one of the listed values.
namespace mgs4e::tool
{
    struct Field
    {
        enum class Type { Bool, Int, Choice };

        const char* section = nullptr;
        const char* key = nullptr;
        Type type = Type::Bool;
        const char* help = "";

        bool defaultBool = false;

        int defaultInt = 0;
        int min = 0;
        int max = 0;

        const char* defaultChoice = nullptr;
        std::vector<const char*> choices;

        // When set, the field is only enabled (Show == false) or shown (Show == true) while
        // the parent field's value is one of parentValues. Bool parents use "true"/"false".
        const char* parentKey = nullptr;
        std::vector<const char*> parentValues;
        bool parentShows = false;

        // The value written to a fresh settings file, as text.
        std::string DefaultText() const;

        // Whether text is a value this field accepts; used when reading a file by hand-edit.
        bool Accepts(const std::string& text) const;

        static Field Bool(const char* section, const char* key, const char* help, bool def);
        static Field Int(const char* section, const char* key, const char* help, int def, int min, int max);
        static Field Choice(const char* section, const char* key, const char* help, const char* def, std::vector<const char*> choices);

        // The field is enabled only while the parent holds one of the values.
        Field& EnabledWhen(const char* parent, std::vector<const char*> values);
        // The field is shown only while the parent holds one of the values.
        Field& ShownWhen(const char* parent, std::vector<const char*> values);
    };

    struct Group
    {
        const char* title;
        std::vector<Field> fields;
    };

    struct Page
    {
        const char* title;
        std::vector<Group> groups;
    };

    // Everything the tool edits. Pages become notebook tabs, groups become boxes.
    const std::vector<Page>& Pages();

    // Every field on every page, flattened, for lookups by key.
    const std::vector<const Field*>& AllFields();
    const Field* FindField(const std::string& section, const std::string& key);
}
