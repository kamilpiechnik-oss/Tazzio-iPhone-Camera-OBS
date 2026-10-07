cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)
project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static third-party libraries" FORCE)
set(BUILD_SHARED_DEPS_LIBS OFF CACHE BOOL "Build static libdatachannel dependencies" FORCE)
set(OPENSSL_USE_STATIC_LIBS ON CACHE BOOL "Link OpenSSL statically" FORCE)
set(OPENSSL_MSVC_STATIC_RT OFF CACHE BOOL "Use dynamic MSVC runtime" FORCE)
set(NO_EXAMPLES ON CACHE BOOL "Disable examples" FORCE)
set(NO_TESTS ON CACHE BOOL "Disable tests" FORCE)
set(NO_MEDIA OFF CACHE BOOL "Enable media transport" FORCE)
set(NO_WEBSOCKET OFF CACHE BOOL "Enable WebSocket support" FORCE)
set(WARNINGS_AS_ERRORS OFF CACHE BOOL "Do not promote dependency warnings" FORCE)
add_subdirectory(vendor/libdatachannel EXCLUDE_FROM_ALL)

include(compilerconfig)
include(defaults)
include(helpers)

if(MSVC AND TARGET usrsctp)
  target_compile_options(usrsctp PRIVATE /WX-)
  set_property(TARGET usrsctp PROPERTY COMPILE_WARNING_AS_ERROR OFF)
endif()

add_library(${CMAKE_PROJECT_NAME} MODULE)
find_package(libobs REQUIRED)
find_package(obs-frontend-api REQUIRED)
find_package(Qt6 COMPONENTS Widgets Core Network REQUIRED)
find_package(FFmpeg 6 REQUIRED COMPONENTS avcodec avutil swscale)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
  src/module.cpp
  src/camera-source.cpp
  src/camera-session.cpp
  src/camera-dock.cpp
  src/peer-transport.cpp
  src/media-decoder.cpp
  vendor/qrcodegen/qrcodegen.cpp
)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE src vendor/qrcodegen)
target_compile_features(${CMAKE_PROJECT_NAME} PRIVATE cxx_std_20)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE
  OBS::libobs OBS::obs-frontend-api Qt6::Core Qt6::Widgets Qt6::Network
  LibDataChannel::LibDataChannelStatic FFmpeg::avcodec FFmpeg::avutil FFmpeg::swscale
)
set_target_properties(${CMAKE_PROJECT_NAME} PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
