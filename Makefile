# crête — Zero-dependency Dynamic Range Meter
# Build: make
# Build with custom version: make VERSION=0.2.0
# Clean: make clean

CXX       ?= g++
CXXFLAGS   = -std=c++17 -Wall -Wextra -Wpedantic
LDFLAGS    =

VERSION   ?= 0.1.1
CXXFLAGS  += -DCRETE_VERSION='"$(VERSION)"'

SRC        = main.cpp
TARGET     = crete

PREFIX    ?= /usr/local

# Default: optimized build
all: release

release: CXXFLAGS += -O2 -DNDEBUG
release: $(TARGET)

debug: CXXFLAGS += -O0 -g -fsanitize=address,undefined
debug: LDFLAGS  += -fsanitize=address,undefined
debug: $(TARGET)

$(TARGET): $(SRC) analysis.hpp audio.hpp
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: release
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

.PHONY: all release debug clean install
