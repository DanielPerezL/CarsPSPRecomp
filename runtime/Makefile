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
CFLAGS   := -O2 -g -Wall -Wextra -fno-strict-aliasing -I include -I src
CXXFLAGS := -std=c++17 $(CFLAGS)

GEN_SRC := $(wildcard $(GEN)/*.c)
RT_SRC  := $(wildcard src/*.cpp)
GEN_OBJ := $(patsubst $(GEN)/%.c,$(OBJ)/gen/%.o,$(GEN_SRC))
RT_OBJ  := $(patsubst src/%.cpp,$(OBJ)/rt/%.o,$(RT_SRC))

all: $(BIN)

$(BIN): $(GEN_OBJ) $(RT_OBJ)
	$(CXX) -o $@ $^ -lm

$(OBJ)/gen/%.o: $(GEN)/%.c include/recomp.h
	@mkdir -p $(dir $@)
	$(CC) $(GENFLAGS) -c $< -o $@

$(OBJ)/rt/%.o: src/%.cpp $(wildcard src/*.h) include/recomp.h
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ) $(BIN)

.PHONY: all clean
