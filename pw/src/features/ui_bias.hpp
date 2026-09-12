#pragma once

// Live editing of UI elements is part of the private ultrawide module (external\ultrawide,
// under pw\). When the module is checked out, the Lab census calls into it; otherwise these
// stubs make the calls vanish and the live commands it would handle are reported as unknown.
#if __has_include("../../../external/ultrawide/pw/src/lab/ui_bias.hpp")
#include "../../../external/ultrawide/pw/src/lab/ui_bias.hpp"
#else
namespace UiBias
{
    inline bool Command(const std::string&) { return false; }
    inline bool Active() { return false; }
    inline bool GameplaySeen() { return false; }
    inline void OnConstantUnmap(void*, size_t) {}
    inline void OnVertexUnmap(const std::string&, void*, size_t) {}
    inline uint64_t AppliedVertex() { return 0; }
    inline uint64_t AppliedTranslation() { return 0; }
}
#endif
