#pragma once

// The wide and tall canvas (the aspect ratio feature) is part of the private ultrawide module
// (external\ultrawide, under pw\src\fixes). When the module is checked out its header is
// included here and MGSPWEnabler.vcxproj compiles its source; when absent these stubs make
// the calls vanish and the Aspect Ratio setting does nothing beyond the sizes.
#if __has_include("../../../external/ultrawide/pw/src/fixes/canvas.hpp")
#include "../../../external/ultrawide/pw/src/fixes/canvas.hpp"
#else
namespace Canvas
{
    inline void Configure(int, int, int) {}
    inline bool Active() { return false; }
    inline int Units() { return 480; }
    inline int HeightUnits() { return 272; }
    inline bool OnConstantUnmap(void*, size_t) { return false; }
    inline void OnVertexUnmap(void*, size_t) {}
}
#endif
