# Compila el runtime y el código generado por tools/recomp/recomp.py.
# Uso (desde la raíz del repo): make -C runtime -j$(nproc)
ROOT    := ..
GEN     := $(ROOT)/build/gen
OBJ     := $(ROOT)/build/obj
BIN     := $(ROOT)/build/cars

CC      ?= gcc
CXX     ?= g++
# El código generado es enorme y plano: -O1 compila rápido y rinde bien.
GENFLAGS := -O1 -fno-strict-aliasing -fwrapv -w -I include -I $(GEN)
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)
AV_CFLAGS  := $(shell pkg-config --cflags libavcodec libavutil)
AV_LIBS    := $(shell pkg-config --libs libavcodec libavutil)
CFLAGS   := -O2 -g -Wall -Wextra -fno-strict-aliasing -I include -I src -I third_party -I third_party/imgui $(SDL_CFLAGS) $(AV_CFLAGS)
CXXFLAGS := -std=c++17 $(CFLAGS)

GEN_SRC := $(wildcard $(GEN)/*.c)
RT_SRC  := $(wildcard src/*.cpp src/gpu/*.cpp src/platform/*.cpp src/audio/*.cpp src/media/*.cpp src/ui/*.cpp)
# Dear ImGui (interfaz propia): código de terceros, sin avisos
IMGUI_SRC := $(wildcard third_party/imgui/*.cpp) third_party/imgui/backends/imgui_impl_sdl2.cpp \
             third_party/imgui/backends/imgui_impl_opengl3.cpp
IMGUI_OBJ := $(patsubst third_party/%.cpp,$(OBJ)/third_party/%.o,$(IMGUI_SRC))
GEN_OBJ := $(patsubst $(GEN)/%.c,$(OBJ)/gen/%.o,$(GEN_SRC))
RT_OBJ  := $(patsubst src/%.cpp,$(OBJ)/rt/%.o,$(RT_SRC))

all: $(BIN)

$(BIN): $(GEN_OBJ) $(RT_OBJ) $(IMGUI_OBJ)
	$(CXX) -o $@ $^ $(SDL_LIBS) $(AV_LIBS) -lm -ldl

$(OBJ)/gen/%.o: $(GEN)/%.c include/recomp.h
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -c $< -o $@

$(OBJ)/rt/%.o: src/%.cpp $(wildcard src/*.h src/*/*.h) include/recomp.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OBJ)/third_party/%.o: third_party/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -w -I third_party/imgui $(SDL_CFLAGS) -DIMGUI_DISABLE_DEMO_WINDOWS -c $< -o $@

clean:
	rm -rf $(OBJ) $(BIN)

.PHONY: all clean
