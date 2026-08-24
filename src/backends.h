#pragma once

namespace gamescope
{
    // Backend enum.
    enum GamescopeBackend
    {
        Auto,
        DRM,
        Headless,
    };

    // Backend forward declarations.
    class CDRMBackend;
    class CHeadlessBackend;
}
