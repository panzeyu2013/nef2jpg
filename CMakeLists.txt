cmake_minimum_required(VERSION 3.24)
project(nef2jpg VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(APPLE)
  # 固定部署目标，避免随本机构建 SDK 浮动
  set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "macOS deployment target")
endif()

option(NEF2JPG_WITH_EXIF "Build with EXIF writing support (exiv2)" ON)
option(NEF2JPG_BUILD_BENCH "Build bench/bench_sdk.cpp" OFF)

# Homebrew 前缀（Apple Silicon: /opt/homebrew, Intel: /usr/local）
if(NOT DEFINED CMAKE_PREFIX_PATH AND EXISTS "/opt/homebrew")
  list(APPEND CMAKE_PREFIX_PATH "/opt/homebrew")
endif()

# ------------------------------------------------------------------
# libjpeg-turbo（brew: brew install libjpeg-turbo）
# ------------------------------------------------------------------
find_package(JPEG REQUIRED)

# ------------------------------------------------------------------
# exiv2（brew: brew install exiv2；可选）
# ------------------------------------------------------------------
if(NEF2JPG_WITH_EXIF)
  find_package(exiv2 CONFIG REQUIRED)
endif()

# ------------------------------------------------------------------
# Nikon Image SDK (macOS only)
# ------------------------------------------------------------------
set(NIKON_SDK_DIR "" CACHE PATH "Path to Nikon Image SDK Mac folder (contains Include/, Sample/Lib/release/, Profiles/)")
if(NOT NIKON_SDK_DIR)
  set(NIKON_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Nikon_image/Library/Mac")
endif()

if(APPLE AND NOT EXISTS "${NIKON_SDK_DIR}/Include/Nkfl_Interface.h")
  message(FATAL_ERROR
    "Nikon Image SDK not found at: ${NIKON_SDK_DIR}\n"
    "Set -DNIKON_SDK_DIR=<path> to the SDK Mac folder "
    "(e.g. .../Nikon_image/Library/Mac).")
endif()

add_executable(nef2jpg
  src/main.cpp
  src/cli.cpp
  src/decoder.cpp
  src/decoder_sdk.cpp
  src/pipeline.cpp
  src/jpeg_encoder.cpp
  src/resize.cpp
  src/exif.cpp
  src/util.cpp
)

target_include_directories(nef2jpg PRIVATE src)
target_compile_definitions(nef2jpg PRIVATE
  NEF2JPG_VERSION="${PROJECT_VERSION}")
if(NEF2JPG_WITH_EXIF)
  target_compile_definitions(nef2jpg PRIVATE NEF2JPG_WITH_EXIF)
endif()

target_compile_options(nef2jpg PRIVATE -Wall -Wextra)
if(APPLE)
  target_compile_options(nef2jpg PRIVATE -Wno-deprecated-declarations)
endif()

if(APPLE)
  # libjpeg 静态链接（brew 提供 libjpeg.a），减少运行时依赖
  find_library(JPEG_STATIC_LIB NAMES libjpeg.a PATHS /opt/homebrew/lib /usr/local/lib)
  if(JPEG_STATIC_LIB)
    target_link_libraries(nef2jpg PRIVATE "${JPEG_STATIC_LIB}")
  else()
    target_link_libraries(nef2jpg PRIVATE JPEG::JPEG)
  endif()
else()
  target_link_libraries(nef2jpg PRIVATE JPEG::JPEG)
endif()

if(NEF2JPG_WITH_EXIF)
  target_link_libraries(nef2jpg PRIVATE exiv2lib)
endif()

if(APPLE)
  target_compile_definitions(nef2jpg PRIVATE NEF2JPG_USE_SDK=1)
  target_include_directories(nef2jpg PRIVATE "${NIKON_SDK_DIR}/Include")
  target_link_libraries(nef2jpg PRIVATE
    "-L${NIKON_SDK_DIR}/Sample/Lib/release" -lImgSDK
    "-framework Carbon"
    "-framework CoreFoundation"
    "-framework CoreGraphics")
  target_link_options(nef2jpg PRIVATE
    "-Wl,-rpath,@executable_path/lib")

  # ---- copy SDK runtime files + 自包含动态库 fixup ----
  set(SDK_LIB_DIR "${NIKON_SDK_DIR}/Sample/Lib/release")
  set(SDK_RES_DIR "${NIKON_SDK_DIR}/Sample/Resources")
  set(SDK_DYLIBS
    libImgSDK.dylib libRCSigProc.dylib libtbb.dylib libtbbmalloc.dylib
    libboost_atomic-clang-darwin150-mt-1_82.dylib
    libboost_filesystem-clang-darwin150-mt-1_82.dylib
    libboost_system-clang-darwin150-mt-1_82.dylib
    libboost_thread-clang-darwin150-mt-1_82.dylib)
  set(SDK_DYLIBS_FULL "")
  foreach(lib ${SDK_DYLIBS})
    list(APPEND SDK_DYLIBS_FULL "${SDK_LIB_DIR}/${lib}")
  endforeach()
  add_custom_command(TARGET nef2jpg POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:nef2jpg>/lib"
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:nef2jpg>/Contents/Resources"
    COMMAND ${CMAKE_COMMAND} -E remove_directory
            "$<TARGET_FILE_DIR:nef2jpg>/lib/Elm.framework"
    COMMAND cp -R "${SDK_LIB_DIR}/Elm.framework" "$<TARGET_FILE_DIR:nef2jpg>/lib/"
    COMMAND ${CMAKE_COMMAND} -E copy
            "${SDK_RES_DIR}/prm.bin"
            "$<TARGET_FILE_DIR:nef2jpg>/Contents/Resources/prm.bin"
    COMMAND ${CMAKE_COMMAND} -E copy
            ${SDK_DYLIBS_FULL}
            "$<TARGET_FILE_DIR:nef2jpg>/lib/"
    COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fixup_macos.sh"
            "$<TARGET_FILE_DIR:nef2jpg>/nef2jpg"
            "$<TARGET_FILE_DIR:nef2jpg>/lib"
    VERBATIM)

  # 完整打包 build 产物（含 fixup 后的 exiv2 依赖）
  install(TARGETS nef2jpg RUNTIME DESTINATION .)
  install(DIRECTORY "$<TARGET_FILE_DIR:nef2jpg>/lib" DESTINATION .)
  install(FILES "$<TARGET_FILE_DIR:nef2jpg>/Contents/Resources/prm.bin"
          DESTINATION Contents/Resources)
  install(DIRECTORY "${NIKON_SDK_DIR}/Profiles" DESTINATION .)
  install(FILES README.md DESTINATION .)
  install(PROGRAMS scripts/install_profiles.sh DESTINATION scripts)
else()
  target_compile_definitions(nef2jpg PRIVATE NEF2JPG_USE_SDK=0)
  install(TARGETS nef2jpg RUNTIME DESTINATION .)
  install(FILES README.md DESTINATION .)
endif()

if(APPLE AND NEF2JPG_BUILD_BENCH)
  add_executable(bench_sdk bench/bench_sdk.cpp)
  target_include_directories(bench_sdk PRIVATE "${NIKON_SDK_DIR}/Include")
  target_compile_options(bench_sdk PRIVATE -Wall -Wextra)
  target_link_libraries(bench_sdk PRIVATE
    "-L${NIKON_SDK_DIR}/Sample/Lib/release" -lImgSDK
    "-framework Carbon")
endif()
