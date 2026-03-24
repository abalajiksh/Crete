# ============================================================================
# crête — Zero-dependency Dynamic Range Meter
#
# Targets:
#   make            Build CLI only (zero dependencies)
#   make cli        Same as above
#   make gui        Build GUI app (requires SDL2 + OpenGL + Dear ImGui)
#   make all        Build both CLI and GUI
#   make debug      Debug CLI with sanitizers
#   make debug-gui  Debug GUI with sanitizers
#   make clean      Remove all build artifacts
#   make install    Install CLI to $(PREFIX)/bin
#
# Cross-compilation (from Linux):
#   make cli CROSS=x86_64-w64-mingw32-    # Windows 64-bit CLI
#   make gui CROSS=x86_64-w64-mingw32-    # Windows 64-bit GUI
#
# GUI dependencies:
#   Fedora:  sudo dnf install SDL2-devel mesa-libGL-devel
#   Ubuntu:  sudo apt install libsdl2-dev libgl-dev
#   macOS:   brew install sdl2
#   Windows: pacman -S mingw-w64-x86_64-SDL2 (MSYS2)
#   ImGui:   git clone --depth 1 https://github.com/ocornut/imgui.git third_party/imgui
# ============================================================================

# ── Toolchain ───────────────────────────────────────────────────────────────
CROSS     ?=
CXX        = $(CROSS)g++
CXXFLAGS   = -std=c++17 -Wall -Wextra -Wpedantic
LDFLAGS    =

VERSION   ?= 0.3.10
CXXFLAGS  += -DCRETE_VERSION='"$(VERSION)"'

PREFIX    ?= /usr/local
BUILD_DIR  = build

# ── Platform detection ──────────────────────────────────────────────────────
# Detect target platform from compiler or environment
UNAME_S := $(shell uname -s 2>/dev/null || echo Unknown)

# Check if cross-compiling for Windows (MinGW)
ifneq (,$(findstring mingw,$(CROSS)))
    TARGET_OS := Windows
else ifeq ($(OS),Windows_NT)
    TARGET_OS := Windows
else ifeq ($(UNAME_S),Darwin)
    TARGET_OS := Darwin
else
    TARGET_OS := Linux
endif

# ── Platform-specific flags ─────────────────────────────────────────────────
ifeq ($(TARGET_OS),Windows)
    # Windows (native MSYS2/MinGW or cross-compilation)
    EXE_EXT    = .exe
    CLI_LDFLAGS =
    GUI_PLAT_CXXFLAGS = $(shell pkg-config --cflags sdl2 2>/dev/null || echo -I/mingw64/include/SDL2)
    GUI_PLAT_LDFLAGS  = $(shell pkg-config --libs sdl2 2>/dev/null || echo -lmingw32 -lSDL2main -lSDL2) \
                        -lopengl32 -lgdi32 -limm32 -lole32 -loleaut32 -lversion -lsetupapi
    # Static linking for portable binaries (optional, enable with STATIC=1)
    ifeq ($(STATIC),1)
        LDFLAGS   += -static
        GUI_PLAT_LDFLAGS := -static $(GUI_PLAT_LDFLAGS) -lwinmm -ldxguid
    endif
else ifeq ($(TARGET_OS),Darwin)
    # macOS
    EXE_EXT    =
    CLI_LDFLAGS =
    GUI_PLAT_CXXFLAGS = $(shell pkg-config --cflags sdl2 2>/dev/null)
    GUI_PLAT_LDFLAGS  = $(shell pkg-config --libs sdl2 2>/dev/null) \
                        -framework OpenGL -lpthread
else
    # Linux (default)
    EXE_EXT    =
    CLI_LDFLAGS = -lpthread
    GUI_PLAT_CXXFLAGS = $(shell pkg-config --cflags sdl2 2>/dev/null)
    GUI_PLAT_LDFLAGS  = $(shell pkg-config --libs sdl2 2>/dev/null) \
                        -lGL -lpthread
endif

# ── CLI target (zero dependencies) ─────────────────────────────────────────
CLI_SRC    = main.cpp
CLI_TARGET = crete$(EXE_EXT)

# ── GUI target ──────────────────────────────────────────────────────────────
IMGUI_DIR  = third_party/imgui
GUI_SRC    = gui_main.cpp
GUI_TARGET = crete-gui$(EXE_EXT)

IMGUI_SRCS = $(IMGUI_DIR)/imgui.cpp \
             $(IMGUI_DIR)/imgui_draw.cpp \
             $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_sdl2.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

IMGUI_OBJS = $(patsubst $(IMGUI_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(IMGUI_SRCS))

GUI_CXXFLAGS = $(CXXFLAGS) \
               -I$(IMGUI_DIR) \
               -I$(IMGUI_DIR)/backends \
               $(GUI_PLAT_CXXFLAGS)

GUI_LDFLAGS  = $(LDFLAGS) $(GUI_PLAT_LDFLAGS)

# ── Default target ──────────────────────────────────────────────────────────
.DEFAULT_GOAL := cli

cli: release

all: release gui

# ── CLI builds ──────────────────────────────────────────────────────────────
release: CXXFLAGS += -O2 -DNDEBUG
release: $(CLI_TARGET)

debug: CXXFLAGS += -O0 -g -fsanitize=address,undefined
debug: LDFLAGS  += -fsanitize=address,undefined
debug: $(CLI_TARGET)

$(CLI_TARGET): $(CLI_SRC) analysis.hpp audio.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(CLI_SRC) $(LDFLAGS) $(CLI_LDFLAGS)

# ── GUI builds ──────────────────────────────────────────────────────────────
gui: GUI_CXXFLAGS += -O2 -DNDEBUG
gui: $(GUI_TARGET)

debug-gui: GUI_CXXFLAGS += -O0 -g -fsanitize=address,undefined
debug-gui: GUI_LDFLAGS  += -fsanitize=address,undefined
debug-gui: $(GUI_TARGET)

$(GUI_TARGET): $(GUI_SRC) $(IMGUI_OBJS) analysis.hpp audio.hpp file_dialog.hpp
	$(CXX) $(GUI_CXXFLAGS) -o $@ $(GUI_SRC) $(IMGUI_OBJS) $(GUI_LDFLAGS)

# ── ImGui object files ──────────────────────────────────────────────────────
$(BUILD_DIR)/%.o: $(IMGUI_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(GUI_CXXFLAGS) -c -o $@ $<

# ── Install ─────────────────────────────────────────────────────────────────
install: release
	install -Dm755 $(CLI_TARGET) $(DESTDIR)$(PREFIX)/bin/crete$(EXE_EXT)

install-gui: gui
	install -Dm755 $(GUI_TARGET) $(DESTDIR)$(PREFIX)/bin/crete-gui$(EXE_EXT)

# ── Clean ───────────────────────────────────────────────────────────────────
clean:
	rm -f crete crete.exe crete-gui crete-gui.exe
	rm -rf $(BUILD_DIR)

# ── Setup helper ────────────────────────────────────────────────────────────
setup-imgui:
	@echo "Downloading Dear ImGui..."
	@mkdir -p third_party
	git clone --depth 1 https://github.com/ocornut/imgui.git $(IMGUI_DIR) 2>/dev/null || \
		echo "ImGui already present in $(IMGUI_DIR)"
	@echo "Done. Run 'make gui' to build."

.PHONY: all cli release debug gui debug-gui clean install install-gui setup-imgui
