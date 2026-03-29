APP_DEBUG := build/app_debug
TARGET    := build/app
BUILD_DIR := build

CC  := clang
CXX := clang++

# -----------------------------
# Sources
# -----------------------------

SRC_C := main.c ext.c vk.c helpers.c offset_allocator.c passes.c renderer.c text_baker.c text_system.c gltfloader.c



SRC_CPP := vma.cpp \
           $(wildcard external/meshoptimizer/src/*.cpp) \
           external/cimgui/cimgui.cpp \
           external/cimgui/cimgui_impl.cpp \
           external/cimgui/imgui/imgui.cpp \
           external/cimgui/imgui/imgui_draw.cpp \
           external/cimgui/imgui/imgui_demo.cpp \
           external/cimgui/imgui/imgui_tables.cpp \
           external/cimgui/imgui/imgui_widgets.cpp \
           external/cimgui/imgui/backends/imgui_impl_glfw.cpp \
           external/cimgui/imgui/backends/imgui_impl_vulkan.cpp \
           external/tracy/public/TracyClient.cpp
OBJ := $(addprefix $(BUILD_DIR)/, $(SRC_C:.c=.o) $(SRC_CPP:.cpp=.o))

# -----------------------------
# Includes
# -----------------------------

INCLUDES := -Iexternal/cimgui \
            -Iexternal/cimgui/imgui \
            -Iexternal/cimgui/imgui/backends

# -----------------------------
# Base Flags
# -----------------------------

BASE_CFLAGS   := -std=gnu99
BASE_CXXFLAGS := -std=c++17 -w -fno-common $(INCLUDES) \
                 -DIMGUI_IMPL_VULKAN_NO_PROTOTYPES \
                 -DIMGUI_IMPL_API='extern "C"'

# -----------------------------
# Debug Flags
# -----------------------------

DEBUG_FLAGS := -O0 -g -ggdb -fno-omit-frame-pointer  -DDEBUG      -DTRACY_ENABLE 

# -----------------------------
# Aggressive Release Flags
# -----------------------------

RELEASE_FLAGS := -O3 -march=native -mtune=native \
                 -fomit-frame-pointer \
                 -fno-math-errno \
                 -fno-trapping-math \
                 -fstrict-aliasing \
                 -fno-semantic-interposition \
                 -DNDEBUG  -DTRACY_ENABLE 


# -----------------------------
# Libraries
# -----------------------------

LIBS := -lvulkan -lm -lglfw \
        -lX11 -lXi -lXrandr -lXcursor -lXinerama \
        -ldl -lpthread

# -----------------------------
# Default = Debug
# -----------------------------

CFLAGS   := $(BASE_CFLAGS) $(DEBUG_FLAGS)
CXXFLAGS := $(BASE_CXXFLAGS) $(DEBUG_FLAGS)
LDFLAGS  :=

# -----------------------------
# Build Targets
# -----------------------------

all: $(TARGET)

$(TARGET): $(OBJ)
	@echo Linking $@
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

# -----------------------------
# Compilation
# -----------------------------

$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	@echo Compiling C $<
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo Compiling C++ $<
	$(CXX) $(CXXFLAGS) -c $< -o $@

# -----------------------------
# Release Build
# -----------------------------

release: CFLAGS   := $(BASE_CFLAGS) $(RELEASE_FLAGS)
release: CXXFLAGS := $(BASE_CXXFLAGS) $(RELEASE_FLAGS)
release: LDFLAGS  :=  -O3
release:  $(TARGET)

# -----------------------------
# Clean
# -----------------------------

clean:
	@echo Cleaning...
	rm -rf $(BUILD_DIR) $(TARGET)

.PHONY: all clean release
