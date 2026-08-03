#pragma once

#include "ConfigSerialization.h"

namespace IniUtils {
    struct IniValue;
}

BEGIN_NAMESPACE(ConfigSerialization)

// N.B: must ONLY contain 'ConfigField' entries!
struct Config_Graphics {
#if PSYDOOM_3DS
    ConfigField     logicalDisplayWidth;
    ConfigField     topOverscanPixels;
    ConfigField     bottomOverscanPixels;
    ConfigField     enhanceWallDrawPrecision;
    ConfigField     floorRenderGapFix;
    ConfigField     useExtendedAutomapColors;
#else
    ConfigField     fullscreen;
    ConfigField     enableVSync;
    ConfigField     outputResolutionW;
    ConfigField     outputResolutionH;
    ConfigField     outputDisplayIndex;
    ConfigField     exclusiveFullscreenMode;
    ConfigField     antiAliasingMultisamples;
    ConfigField     vulkanRenderHeight;
    ConfigField     vulkanPixelStretch;
    ConfigField     vulkanTripleBuffer;
    ConfigField     vulkanDrawExtendedStatusBar;
    ConfigField     vulkanWidescreenEnabled;
    ConfigField     useVulkan32BitShading;
    ConfigField     disableVulkanRenderer;
    ConfigField     logicalDisplayWidth;
    ConfigField     topOverscanPixels;
    ConfigField     bottomOverscanPixels;
    ConfigField     enhanceWallDrawPrecision;
    ConfigField     floorRenderGapFix;
    ConfigField     skyLeakFix;
    ConfigField     vulkanBrightenAutomap;
    ConfigField     useExtendedAutomapColors;
    ConfigField     vramSizeInMegabytes;
    ConfigField     vulkanPreferredDevicesRegex;
#endif

    inline ConfigFieldList getFieldList() noexcept {
        static_assert(sizeof(*this) % sizeof(ConfigField) == 0);
        return { (ConfigField*) this, sizeof(*this) / sizeof(ConfigField) };
    };
};

extern Config_Graphics gConfig_Graphics;

void initCfgSerialization_Graphics() noexcept;

END_NAMESPACE(ConfigSerialization)


