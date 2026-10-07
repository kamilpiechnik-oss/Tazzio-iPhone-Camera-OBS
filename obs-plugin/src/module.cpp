#include <obs-module.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <cstdint>

#ifdef _WIN32
#include <Windows.h>
#endif

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
#ifdef _WIN32
    HMODULE module_handle{};
    wchar_t module_path[MAX_PATH]{};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(reinterpret_cast<std::uintptr_t>(&obs_module_load)),
                          &module_handle) &&
        GetModuleFileNameW(module_handle, module_path, MAX_PATH)) {
        const auto runtime_path = QFileInfo(QString::fromWCharArray(module_path))
                                      .absoluteDir()
                                      .filePath(QStringLiteral("tazzio-iphone-camera-runtime"));
        if (QDir(runtime_path).exists())
            QCoreApplication::addLibraryPath(runtime_path);
    }
#endif
    obs_register_source(&tazzio_iphone_camera_source_info);
    register_iphone_camera_dock();
    blog(LOG_INFO, "[Tazzio iPhone Camera] loaded version 1.0.2");
    return true;
}
