cmake_minimum_required(VERSION 3.20)

project(JdkManagement VERSION 1.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(jkm
    src/main.cpp
    src/app/app.cpp
    src/core/models.cpp
    src/core/paths.cpp
    src/core/store.cpp
    src/infrastructure/audit.cpp
    src/infrastructure/logger.cpp
    src/infrastructure/process.cpp
    src/infrastructure/platform_windows.cpp
    src/providers/download_helpers.cpp
    src/providers/temurin.cpp
    src/providers/python_runtime.cpp
    src/providers/tool_runtime.cpp
    src/resources/jkm.rc
)

target_include_directories(jkm PRIVATE src)

if (WIN32)
    target_compile_definitions(jkm PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(jkm PRIVATE advapi32 user32)
endif()


add_executable(jkm-setup
    src/installer/installer.cpp
)
target_include_directories(jkm-setup PRIVATE src)

add_executable(jkm-uninstall
    src/installer/installer.cpp
)
target_include_directories(jkm-uninstall PRIVATE src)

if (WIN32)
    target_compile_definitions(jkm-setup PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(jkm-setup PRIVATE advapi32 user32)
    target_compile_definitions(jkm-uninstall PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX JKM_UNINSTALLER)
    target_link_libraries(jkm-uninstall PRIVATE advapi32 user32)
endif()
