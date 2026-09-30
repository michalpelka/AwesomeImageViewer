cmake_minimum_required(VERSION 3.24)
project(PhotoViewer LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# --- raylib -------------------------------------------------------------------
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 6.0
    GIT_SHALLOW TRUE)

# --- Dear ImGui (docking) -----------------------------------------------------
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.92.9b-docking
    GIT_SHALLOW TRUE)

# --- rlImGui (raylib backend for ImGui); SOURCE_SUBDIR stops add_subdirectory --
FetchContent_Declare(rlimgui
    GIT_REPOSITORY https://github.com/raylib-extras/rlImGui.git
    GIT_TAG 1550009359ad975927f7f0e4a3f47e3f27123ea9
    SOURCE_SUBDIR _none_)

FetchContent_MakeAvailable(raylib imgui rlimgui)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)

add_library(imgui_rl STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${rlimgui_SOURCE_DIR}/rlImGui.cpp)
target_include_directories(imgui_rl PUBLIC ${imgui_SOURCE_DIR} ${rlimgui_SOURCE_DIR})
target_compile_definitions(imgui_rl PUBLIC NO_FONT_AWESOME)
target_link_libraries(imgui_rl PUBLIC raylib)

add_executable(PhotoViewer
    src/main.cpp
    src/image_doc.cpp
    src/exif.cpp)
if(APPLE)
    target_sources(PhotoViewer PRIVATE src/mac_gestures.mm)
else()
    file(WRITE ${CMAKE_BINARY_DIR}/mac_gestures_stub.cpp
        "#include \"mac_gestures.hpp\"\nvoid macInstallGestures() {}\nfloat macConsumeMagnify() { return 0; }\n")
    target_sources(PhotoViewer PRIVATE ${CMAKE_BINARY_DIR}/mac_gestures_stub.cpp)
    target_include_directories(PhotoViewer PRIVATE src)
endif()
target_link_libraries(PhotoViewer PRIVATE imgui_rl raylib ${OpenCV_LIBS})
if(MSVC)
    target_compile_options(PhotoViewer PRIVATE /utf-8)  # UTF-8 string literals (×, °, …)
endif()
if(APPLE)
    target_link_libraries(PhotoViewer PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
endif()
