cmake_minimum_required(VERSION 3.28)
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
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)  # keep raylib's install rules out of `cmake --install`

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

add_executable(PhotoViewer MACOSX_BUNDLE
    src/main.cpp
    src/image_doc.cpp
    src/exif.cpp)
if(APPLE)
    target_sources(PhotoViewer PRIVATE src/mac_platform.mm cmake/AppIcon.icns)  # icon: scripts/make_icon.py
    set_source_files_properties(cmake/AppIcon.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(PhotoViewer PROPERTIES
        MACOSX_BUNDLE_ICON_FILE AppIcon.icns
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/cmake/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER com.michalpelka.photoviewer
        MACOSX_BUNDLE_SHORT_VERSION_STRING 0.1.0
        MACOSX_BUNDLE_BUNDLE_VERSION 1)
else()
    file(WRITE ${CMAKE_BINARY_DIR}/mac_platform_stub.cpp
        "#include \"mac_platform.hpp\"\n"
        "void macInstallOpenHandler() {}\n"
        "std::vector<std::string> macTakeOpenedFiles() { return {}; }\n"
        "void macInstallGestures() {}\n"
        "float macConsumeMagnify() { return 0; }\n")
    target_sources(PhotoViewer PRIVATE ${CMAKE_BINARY_DIR}/mac_platform_stub.cpp)
    target_include_directories(PhotoViewer PRIVATE src)
endif()
target_link_libraries(PhotoViewer PRIVATE imgui_rl raylib ${OpenCV_LIBS})
if(MSVC)
    target_compile_options(PhotoViewer PRIVATE /utf-8)  # UTF-8 string literals (×, °, …)
endif()
if(APPLE)
    target_link_libraries(PhotoViewer PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
endif()

# `sudo cmake --install build` -> /Applications/PhotoViewer.app on macOS, <prefix>/bin elsewhere.
set(PHOTOVIEWER_MACOS_APP_DIR "/Applications" CACHE PATH "Where the macOS .app bundle is installed")
install(TARGETS PhotoViewer
    BUNDLE DESTINATION ${PHOTOVIEWER_MACOS_APP_DIR}
    RUNTIME DESTINATION bin)

if(UNIX AND NOT APPLE)
    # .desktop entry + icon so `scripts/linux-set-default.sh` can register PhotoViewer via xdg-mime.
    include(GNUInstallDirs)
    set(PHOTOVIEWER_BIN "${CMAKE_INSTALL_FULL_BINDIR}/PhotoViewer")
    configure_file(${CMAKE_SOURCE_DIR}/cmake/photoviewer.desktop.in ${CMAKE_BINARY_DIR}/photoviewer.desktop @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/photoviewer.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${CMAKE_SOURCE_DIR}/cmake/AppIcon.png    # scripts/make_icon.py
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps RENAME photoviewer.png)
    # CMAKE_INSTALL_PREFIX is re-evaluated when this runs, so a `cmake --install --prefix ...`
    # or DESTDIR-staged install still targets its own applications dir, not the configure-time one.
    install(CODE "
        find_program(PHOTOVIEWER_UPDATE_DESKTOP_DB update-desktop-database)
        if(PHOTOVIEWER_UPDATE_DESKTOP_DB)
            execute_process(COMMAND \"\${PHOTOVIEWER_UPDATE_DESKTOP_DB}\"
                \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/applications\")
        endif()
    ")
endif()
