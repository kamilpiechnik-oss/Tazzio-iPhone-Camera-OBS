#include <obs-module.h>

#include "camera-dock.hpp"
#include "camera-source.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("tazzio-iphone-camera", "pl-PL")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Tazzio iPhone Camera for OBS";
}

bool obs_module_load(void)
{
    obs_register_source(&tazzio_iphone_camera_source_info);
    register_iphone_camera_dock();
    blog(LOG_INFO, "[Tazzio iPhone Camera] loaded version 1.0.0");
    return true;
}
