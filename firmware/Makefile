# Pruebas de la lógica pura del firmware, ejecutadas en el host.
# No requieren ESP32, PlatformIO ni sensores conectados.
#
#   make test

CXX ?= g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O1 -g

.PHONY: test limpiar
test: build/pruebas
	@./build/pruebas

build/pruebas: test/nativo/pruebas.cpp \
               src/sensores/filtros.h src/sensores/calibracion.h \
               src/sensores/canal.h src/modelo/contrato.h
	@mkdir -p build
	@$(CXX) $(CXXFLAGS) -o $@ test/nativo/pruebas.cpp

limpiar:
	@rm -rf build
