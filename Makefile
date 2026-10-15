CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CXXFLAGS += $(shell pkg-config --cflags sdl2 freetype2)
# third_party holds one vendored header (stb_image, for the piece images):
# a system include, so whatever it contains never shows up as warnings here.
CXXFLAGS += -isystem third_party
LDLIBS   += $(shell pkg-config --libs sdl2 freetype2) -lpthread

# X11 is optional: it feeds the screen overlay (root-window capture and the
# shaped window the advice is drawn on). Without the headers the overlay simply
# reports that it cannot run, and everything else still builds.
HAVE_X11 := $(shell test -f /usr/include/X11/Xlib.h && echo 1)
ifeq ($(HAVE_X11),1)
CXXFLAGS += -DHAVE_X11
LDLIBS   += -lX11 -lXext
endif

SRC := $(wildcard src/*.cpp)
OBJ := $(patsubst src/%.cpp,build/%.o,$(SRC))
DEP := $(OBJ:.o=.d)
BIN := chess

TESTBIN := build/engine_test
SCREENBIN := build/screen_test
ELOBIN := build/elo_test

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.cpp | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(TESTBIN): tests/engine_test.cpp src/position.cpp src/engine.cpp src/position.h src/engine.h \
		src/levels.h src/common.h | build
	$(CXX) $(CXXFLAGS) -o $@ tests/engine_test.cpp src/position.cpp src/engine.cpp -lpthread

# Screen overlay: board detection and piece recognition on rendered boards.
$(SCREENBIN): tests/screen_test.cpp src/screen.cpp src/pieces.cpp src/position.cpp \
		src/aiwatch.cpp src/screen.h src/pieces.h src/position.h src/aiwatch.h src/common.h | build
	$(CXX) $(CXXFLAGS) -o $@ tests/screen_test.cpp src/screen.cpp src/pieces.cpp src/position.cpp \
		src/aiwatch.cpp $(LDLIBS)

build:
	mkdir -p build

# Elo harness: the built-in bot (at the app's real LEVELS settings) against
# Stockfish's calibrated UCI_Elo ladder. Headless, like the other tests.
$(ELOBIN): tests/elo_test.cpp src/position.cpp src/engine.cpp src/uci.cpp \
		src/levels.h src/position.h src/engine.h src/uci.h src/common.h | build
	$(CXX) $(CXXFLAGS) -o $@ tests/elo_test.cpp src/position.cpp src/engine.cpp src/uci.cpp -lpthread

elo: $(ELOBIN)
	./$(ELOBIN)

clean:
	rm -rf build $(BIN)

# Stockfish lives in-tree (cloned from official-stockfish/Stockfish) and is
# built from source; bin/stockfish is the link the app looks for first.
SF_SRC := stockfish/src
SF_BIN := $(SF_SRC)/stockfish

$(SF_SRC)/Makefile:
	git clone --depth 1 --branch sf_19 https://github.com/official-stockfish/Stockfish.git stockfish

$(SF_BIN): $(SF_SRC)/Makefile
	$(MAKE) -C $(SF_SRC) -j4 build

bin/stockfish: $(SF_BIN)
	mkdir -p bin
	ln -sf ../$(SF_BIN) $@

stockfish: bin/stockfish

# Known perft values from the chess programming wiki
test: $(BIN)
	./$(BIN) --perft 4

# Full headless test suite: SAN, rules, search, perft, then the overlay's eye
check: $(BIN) $(TESTBIN) $(SCREENBIN)
	./$(TESTBIN)
	./$(SCREENBIN)
	./$(BIN) --perft 4
	./$(BIN) --perft 4 --fen "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"

-include $(DEP)

.PHONY: all clean test check stockfish elo
