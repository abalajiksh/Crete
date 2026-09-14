# ============================================================================
# crête — Zero-dependency Dynamic Range Meter
#
# Build tiers:
#   make                  Build zero-dependency CLI (no JSON)
#   make cli              Same as above
#   make cli-json         Build CLI with JSON output (auto-fetches nlohmann/json)
#   make cli-ffmpeg       CLI + bundled compact FFmpeg (decodes ALAC/MP3/AAC/...)
#   make cli-json-ffmpeg  CLI + JSON + compact FFmpeg (the binary the pytest
#                         harness needs: speaks JSON *and* decodes extra formats)
#   make gui              Build GUI app (requires SDL2 + OpenGL + Dear ImGui)
#   make gui-ffmpeg       GUI + bundled compact FFmpeg (binary: crete-gui-ffmpeg;
#                         decodes ALAC/MP3/AAC/... — About lists the codecs)
#   make all              Build cli-json + gui
#   make debug            Debug CLI with sanitizers (no JSON)
#   make debug-json       Debug CLI with sanitizers + JSON
#   make debug-ffmpeg     Debug CLI with sanitizers + FFmpeg
#   make debug-json-ffmpeg  Debug CLI with sanitizers + JSON + FFmpeg
#   make debug-gui        Debug GUI with sanitizers
#   make clean            Remove all build artifacts
#   make install          Install CLI to $(PREFIX)/bin
#
# FFmpeg tiers need the bundled compact FFmpeg AND pkg-config:
#   ./scripts/build_ffmpeg_compact.sh        (or: make setup-ffmpeg)
#   pkg-config:  macOS:  brew install pkg-config
#                Ubuntu: sudo apt install pkg-config
#                Fedora: sudo dnf install pkgconf-pkg-config
#   See exactly what resolves (or what's missing):  make ffmpeg-info
#
# Cross-compilation (from Linux):
#   make cli CROSS=x86_64-w64-mingw32-         # Windows 64-bit CLI
#   make cli-json CROSS=x86_64-w64-mingw32-    # Windows 64-bit CLI + JSON
#   make gui CROSS=x86_64-w64-mingw32-         # Windows 64-bit GUI
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
CC         = $(CROSS)gcc

# Cross-ISA determinism: GCC defaults to -ffp-contract=fast, which fuses a*b+c
# into a single FMA (one rounding) wherever the target has FMA hardware. That
# fires on aarch64 (FMA is ARMv8 baseline) and any x86 build with -mfma/-march,
# but NOT on the generic x86-64 baseline — so the same source yields ULP-
# different results across arches. crête's identity is bit-level reproducibility
# (K-weighting biquads, RMS accumulation), so we pin contraction OFF everywhere:
# every arch computes a*b then +c with separate roundings. No-op on the current
# generic x86-64 build; forces FMA-capable targets (ARM CI) to match it.
FP_DETERMINISM = -ffp-contract=off

CXXFLAGS   = -std=c++17 -Wall -Wextra -Wpedantic $(FP_DETERMINISM)
CFLAGS     = -std=c99 -O2 -Wall -Wextra $(FP_DETERMINISM)
LDFLAGS    =

VERSION   ?= 0.12.1
CXXFLAGS  += -DCRETE_VERSION='"$(VERSION)"'

PREFIX    ?= /usr/local
BUILD_DIR  = build

# ── Vendored libdsddpcm (DSD -> PCM decode kernel, BSD-2-Clause) ─────────────
# Committed under third_party/ (see .gitignore), compiled as a C object and
# statically linked into every crête binary — keeps the single self-contained
# executable. This is crête's sole DSD engine (dsd_lut.hpp is retired).
LIBDSDDPCM_DIR = third_party/libdsddpcm
LIBDSDDPCM_OBJ = $(BUILD_DIR)/dsddpcm.o
CXXFLAGS      += -I$(LIBDSDDPCM_DIR)/include

# ── Platform detection ──────────────────────────────────────────────────────
UNAME_S := $(shell uname -s 2>/dev/null || echo Unknown)

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
    EXE_EXT    = .exe
    CLI_LDFLAGS =
    GUI_PLAT_CXXFLAGS = $(shell pkg-config --cflags sdl2 2>/dev/null || echo -I/mingw64/include/SDL2)
    GUI_PLAT_LDFLAGS  = $(shell pkg-config --libs sdl2 2>/dev/null || echo -lmingw32 -lSDL2main -lSDL2) \
                        -lopengl32 -lgdi32 -limm32 -lole32 -loleaut32 -lversion -lsetupapi
    ifeq ($(STATIC),1)
        LDFLAGS   += -static
        GUI_PLAT_LDFLAGS := -static $(GUI_PLAT_LDFLAGS) -lwinmm -ldxguid
    endif
    CURL = curl -sL
else ifeq ($(TARGET_OS),Darwin)
    EXE_EXT    =
    CLI_LDFLAGS =
    GUI_PLAT_CXXFLAGS = $(shell pkg-config --cflags sdl2 2>/dev/null)
    GUI_PLAT_LDFLAGS  = $(shell pkg-config --libs sdl2 2>/dev/null) \
                        -framework OpenGL -lpthread
    CURL = curl -sL
else
    EXE_EXT    =
    CLI_LDFLAGS = -lpthread
    GUI_PLAT_CXXFLAGS = $(shell pkg-config --cflags sdl2 2>/dev/null)
    GUI_PLAT_LDFLAGS  = $(shell pkg-config --libs sdl2 2>/dev/null) \
                        -lGL -lpthread
    CURL = curl -sL
endif

# ── nlohmann/json (auto-fetched for cli-json builds) ───────────────────────
NLOHMANN_VERSION = v3.12.0
NLOHMANN_HEADER  = third_party/nlohmann/json.hpp
NLOHMANN_URL     = https://github.com/nlohmann/json/releases/download/$(NLOHMANN_VERSION)/json.hpp

# ── CLI target (zero dependencies) ─────────────────────────────────────────
CLI_SRC    = main.cpp
CLI_TARGET = crete$(EXE_EXT)
# FFmpeg tiers emit a distinctly-named binary so the in-house decoder (crete)
# and the FFmpeg-enabled decoder (crete-ffmpeg) never overwrite each other.
FFMPEG_TARGET = crete-ffmpeg$(EXE_EXT)
CLI_HEADERS = analysis.hpp audio.hpp channels.hpp dsd_decode.hpp mqa_detect.hpp cue.hpp \
              $(LIBDSDDPCM_DIR)/include/dsddpcm.h

# ── GUI target ──────────────────────────────────────────────────────────────
IMGUI_DIR  = third_party/imgui
GUI_SRC    = gui_main.cpp
GUI_TARGET = crete-gui$(EXE_EXT)
# Same no-overwrite convention as the CLI tiers: the FFmpeg-enabled GUI gets
# its own binary name.
GUI_FFMPEG_TARGET = crete-gui-ffmpeg$(EXE_EXT)

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

all: release-json gui

# ── CLI builds (zero-dependency, no JSON) ───────────────────────────────────
release: CXXFLAGS += -O2 -DNDEBUG
release: $(CLI_TARGET)

debug: CXXFLAGS += -O0 -g -fsanitize=address,undefined
debug: LDFLAGS  += -fsanitize=address,undefined
debug: $(CLI_TARGET)

$(CLI_TARGET): $(CLI_SRC) $(CLI_HEADERS) $(LIBDSDDPCM_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(CLI_SRC) $(LIBDSDDPCM_OBJ) $(LDFLAGS) $(CLI_LDFLAGS)

# ── CLI builds with JSON (auto-fetches nlohmann/json) ───────────────────────
release-json: CXXFLAGS += -O2 -DNDEBUG -DCRETE_HAS_JSON -I third_party
release-json: $(CLI_TARGET)-json

debug-json: CXXFLAGS += -O0 -g -fsanitize=address,undefined -DCRETE_HAS_JSON -I third_party
debug-json: LDFLAGS  += -fsanitize=address,undefined
debug-json: $(CLI_TARGET)-json

# Alias: "make cli-json" → "make release-json"
cli-json: release-json

$(CLI_TARGET)-json: $(CLI_SRC) $(CLI_HEADERS) $(NLOHMANN_HEADER) $(LIBDSDDPCM_OBJ)
	$(CXX) $(CXXFLAGS) -o $(CLI_TARGET) $(CLI_SRC) $(LIBDSDDPCM_OBJ) $(LDFLAGS) $(CLI_LDFLAGS)

# ── Fetch nlohmann/json header ──────────────────────────────────────────────
$(NLOHMANN_HEADER):
	@echo "Fetching nlohmann/json $(NLOHMANN_VERSION)..."
	@mkdir -p $(dir $@)
	$(CURL) $(NLOHMANN_URL) -o $@
	@echo "Done: $@ ($$(wc -c < $@) bytes)"

# ── GUI builds ──────────────────────────────────────────────────────────────
gui: GUI_CXXFLAGS += -O2 -DNDEBUG
gui: $(GUI_TARGET)

debug-gui: GUI_CXXFLAGS += -O0 -g -fsanitize=address,undefined
debug-gui: GUI_LDFLAGS  += -fsanitize=address,undefined
debug-gui: $(GUI_TARGET)

$(GUI_TARGET): $(GUI_SRC) $(IMGUI_OBJS) analysis.hpp audio.hpp dsd_decode.hpp file_dialog.hpp $(LIBDSDDPCM_OBJ)
	$(CXX) $(GUI_CXXFLAGS) -o $@ $(GUI_SRC) $(IMGUI_OBJS) $(LIBDSDDPCM_OBJ) $(GUI_LDFLAGS)

# ── GUI + compact FFmpeg (ALAC/MP3/AAC/... decode; binary: crete-gui-ffmpeg) ─
# Note: the ImGui objects are shared with the plain gui build; the FFmpeg
# defines are harmless to them but switching between `gui` and `gui-ffmpeg`
# recompiles them (target-specific GUI_CXXFLAGS propagate to prerequisites).
gui-ffmpeg: GUI_CXXFLAGS += -O2 -DNDEBUG -DCRETE_HAS_FFMPEG $(FFMPEG_CFLAGS)
gui-ffmpeg: GUI_LDFLAGS  += $(FFMPEG_LDLIBS)
gui-ffmpeg: $(GUI_FFMPEG_TARGET)

debug-gui-ffmpeg: GUI_CXXFLAGS += -O0 -g -fsanitize=address,undefined -DCRETE_HAS_FFMPEG $(FFMPEG_CFLAGS)
debug-gui-ffmpeg: GUI_LDFLAGS  += -fsanitize=address,undefined $(FFMPEG_LDLIBS)
debug-gui-ffmpeg: $(GUI_FFMPEG_TARGET)

$(GUI_FFMPEG_TARGET): $(GUI_SRC) $(IMGUI_OBJS) analysis.hpp audio.hpp audio_ffmpeg.hpp dsd_decode.hpp file_dialog.hpp $(LIBDSDDPCM_OBJ)
	@$(FFMPEG_PRECHECK)
	$(CXX) $(GUI_CXXFLAGS) -o $@ $(GUI_SRC) $(IMGUI_OBJS) $(LIBDSDDPCM_OBJ) $(GUI_LDFLAGS)

# ── ImGui object files ──────────────────────────────────────────────────────
$(BUILD_DIR)/%.o: $(IMGUI_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(GUI_CXXFLAGS) -c -o $@ $<

# ── Vendored libdsddpcm object (C99, build-config-independent) ───────────────
$(LIBDSDDPCM_OBJ): $(LIBDSDDPCM_DIR)/src/dsddpcm.c \
                   $(LIBDSDDPCM_DIR)/src/dsddpcm_tables.h \
                   $(LIBDSDDPCM_DIR)/include/dsddpcm.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) -I$(LIBDSDDPCM_DIR)/include -c -o $@ $<

# ── dsd2wav (reference-generation tool: DSD -> WAV via crête's decimation) ────
# File target (not phony): `make dsd2wav` on POSIX, `make dsd2wav.exe` on Windows.
DSD2WAV_TARGET = dsd2wav$(EXE_EXT)
$(DSD2WAV_TARGET): dsd2wav.cpp audio.hpp dsd_decode.hpp $(LIBDSDDPCM_OBJ)
	$(CXX) $(CXXFLAGS) -O2 -DNDEBUG -o $@ dsd2wav.cpp $(LIBDSDDPCM_OBJ) $(LDFLAGS) $(CLI_LDFLAGS)

# ── Install ─────────────────────────────────────────────────────────────────
install: release
	install -Dm755 $(CLI_TARGET) $(DESTDIR)$(PREFIX)/bin/crete$(EXE_EXT)

install-json: release-json
	install -Dm755 $(CLI_TARGET) $(DESTDIR)$(PREFIX)/bin/crete$(EXE_EXT)

install-gui: gui
	install -Dm755 $(GUI_TARGET) $(DESTDIR)$(PREFIX)/bin/crete-gui$(EXE_EXT)

install-gui-ffmpeg: gui-ffmpeg
	install -Dm755 $(GUI_FFMPEG_TARGET) $(DESTDIR)$(PREFIX)/bin/crete-gui-ffmpeg$(EXE_EXT)

# ── Clean ───────────────────────────────────────────────────────────────────
clean:
	rm -f crete crete.exe crete-gui crete-gui.exe crete-ffmpeg crete-ffmpeg.exe crete-gui-ffmpeg crete-gui-ffmpeg.exe \
	      dsd2wav dsd2wav.exe
	rm -rf $(BUILD_DIR)

distclean: clean
	rm -rf third_party/nlohmann

# ── Setup helpers ───────────────────────────────────────────────────────────
setup-imgui:
	@echo "Downloading Dear ImGui..."
	@mkdir -p third_party
	git clone --depth 1 https://github.com/ocornut/imgui.git $(IMGUI_DIR) 2>/dev/null || \
		echo "ImGui already present in $(IMGUI_DIR)"
	@echo "Done. Run 'make gui' to build."

setup-nlohmann: $(NLOHMANN_HEADER)
	@echo "nlohmann/json ready at $(NLOHMANN_HEADER)"

setup-ffmpeg:
	./scripts/build_ffmpeg_compact.sh

# ============================================================================
# FFmpeg (optional, compact static build → single self-contained binary)
#
# Prereq: build the bundled compact FFmpeg and have pkg-config available:
#   ./scripts/build_ffmpeg_compact.sh        (or: make setup-ffmpeg)
#
# Flags come from the compact build's .pc files via `pkg-config --static`, so
# the exact per-platform deps (macOS frameworks, -lm/-lpthread, ...) resolve
# automatically — never hardcode them, they differ per machine. If you really
# must bypass pkg-config, override on the command line, e.g.:
#   make cli-ffmpeg FFMPEG_CFLAGS="-I/path/include" \
#                   FFMPEG_LDLIBS="-L/path/lib -lavformat -lavcodec -lswresample -lavutil -framework CoreFoundation ..."
# ============================================================================
FFMPEG_PREFIX    ?= third_party/ffmpeg-compact
FFMPEG_PKGCONFIG  = $(FFMPEG_PREFIX)/lib/pkgconfig
FFMPEG_LIBS       = libavformat libavcodec libswresample libavutil
PKG_CONFIG       ?= pkg-config

# Recursively-expanded (=) on purpose: pkg-config only runs when an FFmpeg
# target is actually built, so plain `make` / `make cli-json` never touch it.
# NOTE: no 2>/dev/null here — a real failure must be visible, not swallowed.
FFMPEG_CFLAGS  = $(shell PKG_CONFIG_PATH=$(FFMPEG_PKGCONFIG) $(PKG_CONFIG) --cflags $(FFMPEG_LIBS))
FFMPEG_LDLIBS  = $(shell PKG_CONFIG_PATH=$(FFMPEG_PKGCONFIG) $(PKG_CONFIG) --libs --static $(FFMPEG_LIBS))

# Loud pre-build guard — fails with an actionable message instead of silently
# dropping the flags (which is what produced a byte-identical no-FFmpeg binary).
FFMPEG_PRECHECK = command -v $(PKG_CONFIG) >/dev/null 2>&1 || { echo "ERROR: pkg-config not found — install it (macOS: brew install pkg-config; Ubuntu: apt install pkg-config; Fedora: dnf install pkgconf-pkg-config)"; exit 1; }; PKG_CONFIG_PATH=$(FFMPEG_PKGCONFIG) $(PKG_CONFIG) --exists $(FFMPEG_LIBS) || { echo "ERROR: compact FFmpeg not found under $(abspath $(FFMPEG_PKGCONFIG)) — run ./scripts/build_ffmpeg_compact.sh (or set FFMPEG_PREFIX=/path)"; exit 1; }

# ── CLI + FFmpeg (no JSON) ──────────────────────────────────────────────────
release-ffmpeg: CXXFLAGS += -O2 -DNDEBUG -DCRETE_HAS_FFMPEG $(FFMPEG_CFLAGS)
release-ffmpeg: LDFLAGS  += $(FFMPEG_LDLIBS)
release-ffmpeg: $(CLI_TARGET)-ffmpeg

debug-ffmpeg: CXXFLAGS += -O0 -g -fsanitize=address,undefined -DCRETE_HAS_FFMPEG $(FFMPEG_CFLAGS)
debug-ffmpeg: LDFLAGS  += -fsanitize=address,undefined $(FFMPEG_LDLIBS)
debug-ffmpeg: $(CLI_TARGET)-ffmpeg

cli-ffmpeg: release-ffmpeg

$(CLI_TARGET)-ffmpeg: $(CLI_SRC) $(CLI_HEADERS) audio_ffmpeg.hpp $(LIBDSDDPCM_OBJ)
	@$(FFMPEG_PRECHECK)
	$(CXX) $(CXXFLAGS) -o $(FFMPEG_TARGET) $(CLI_SRC) $(LIBDSDDPCM_OBJ) $(LDFLAGS) $(CLI_LDFLAGS)

# ── CLI + JSON + FFmpeg (for the pytest harness) ────────────────────────────
release-json-ffmpeg: CXXFLAGS += -O2 -DNDEBUG -DCRETE_HAS_JSON -I third_party -DCRETE_HAS_FFMPEG $(FFMPEG_CFLAGS)
release-json-ffmpeg: LDFLAGS  += $(FFMPEG_LDLIBS)
release-json-ffmpeg: $(CLI_TARGET)-json-ffmpeg

debug-json-ffmpeg: CXXFLAGS += -O0 -g -fsanitize=address,undefined -DCRETE_HAS_JSON -I third_party -DCRETE_HAS_FFMPEG $(FFMPEG_CFLAGS)
debug-json-ffmpeg: LDFLAGS  += -fsanitize=address,undefined $(FFMPEG_LDLIBS)
debug-json-ffmpeg: $(CLI_TARGET)-json-ffmpeg

cli-json-ffmpeg: release-json-ffmpeg

$(CLI_TARGET)-json-ffmpeg: $(CLI_SRC) $(CLI_HEADERS) audio_ffmpeg.hpp $(NLOHMANN_HEADER) $(LIBDSDDPCM_OBJ)
	@$(FFMPEG_PRECHECK)
	$(CXX) $(CXXFLAGS) -o $(FFMPEG_TARGET) $(CLI_SRC) $(LIBDSDDPCM_OBJ) $(LDFLAGS) $(CLI_LDFLAGS)

# ── Diagnose what the FFmpeg tiers resolve to (run this when a build fails) ──
ffmpeg-info:
	@echo "FFMPEG_PREFIX    = $(FFMPEG_PREFIX)"
	@echo "FFMPEG_PKGCONFIG = $(abspath $(FFMPEG_PKGCONFIG))"
	@command -v $(PKG_CONFIG) >/dev/null 2>&1 && echo "pkg-config       = $$(command -v $(PKG_CONFIG))" || echo "pkg-config       = NOT FOUND (macOS: brew install pkg-config)"
	@PKG_CONFIG_PATH=$(FFMPEG_PKGCONFIG) $(PKG_CONFIG) --exists $(FFMPEG_LIBS) 2>/dev/null && echo ".pc files        = found" || echo ".pc files        = NOT FOUND under the path above"
	@echo "cflags           = $(FFMPEG_CFLAGS)"
	@echo "libs (--static)  = $(FFMPEG_LDLIBS)"

.PHONY: all cli cli-json cli-ffmpeg cli-json-ffmpeg \
        release release-json release-ffmpeg release-json-ffmpeg \
        debug debug-json debug-ffmpeg debug-json-ffmpeg \
        gui gui-ffmpeg debug-gui debug-gui-ffmpeg clean distclean \
        install install-json install-gui install-gui-ffmpeg \
        setup-imgui setup-nlohmann setup-ffmpeg ffmpeg-info
