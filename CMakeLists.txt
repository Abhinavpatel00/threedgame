cmake_minimum_required(VERSION 3.20)

project(threedgame LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.3.9
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

find_package(Vulkan REQUIRED)

file(GLOB MESHOPT_SOURCES CONFIGURE_DEPENDS external/meshoptimizer/src/*.cpp)
file(GLOB SOLOUD_CORE_SOURCES CONFIGURE_DEPENDS external/soloud/src/core/*.cpp)
file(GLOB SOLOUD_FILTER_SOURCES CONFIGURE_DEPENDS external/soloud/src/filter/*.cpp)
file(GLOB SOLOUD_WAV_SOURCES CONFIGURE_DEPENDS external/soloud/src/audiosource/wav/*.cpp)

set(SRC_C
    main.c
    ext.c
    vk.c
    helpers.c
    offset_allocator.c
    passes.c
    renderer.c
    text_baker.c
    text_system.c
    gltfloader.c
    input.c
    external/soloud/src/audiosource/wav/stb_vorbis.c
)

set(SRC_CPP
    audio.cpp
    vma.cpp
    external/cimgui/cimgui.cpp
    external/cimgui/cimgui_impl.cpp
    external/cimgui/imgui/imgui.cpp
    external/cimgui/imgui/imgui_draw.cpp
    external/cimgui/imgui/imgui_demo.cpp
    external/cimgui/imgui/imgui_tables.cpp
    external/cimgui/imgui/imgui_widgets.cpp
    external/cimgui/imgui/backends/imgui_impl_glfw.cpp
    external/cimgui/imgui/backends/imgui_impl_vulkan.cpp
    external/tracy/public/TracyClient.cpp
    ${MESHOPT_SOURCES}
    ${SOLOUD_CORE_SOURCES}
    ${SOLOUD_FILTER_SOURCES}
    ${SOLOUD_WAV_SOURCES}
    external/soloud/src/backend/miniaudio/soloud_miniaudio.cpp
)

add_executable(threedgame ${SRC_C} ${SRC_CPP})

target_include_directories(threedgame PRIVATE
    external/cimgui
    external/cimgui/imgui
    external/cimgui/imgui/backends
    external/soloud/include
)

target_compile_definitions(threedgame PRIVATE
    IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    IMGUI_IMPL_API=extern\ \"C\"
    TRACY_ENABLE
    WITH_MINIAUDIO
)

if(MSVC)
    target_compile_options(threedgame PRIVATE /W3 /EHsc)
else()
    target_compile_options(threedgame PRIVATE -Wall -Wextra)
endif()

target_link_libraries(threedgame PRIVATE glfw Vulkan::Vulkan)

if(WIN32)
    target_link_libraries(threedgame PRIVATE user32 gdi32 shell32 winmm)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(threedgame PRIVATE m dl pthread X11 Xi Xrandr Xcursor Xinerama)
endif()
