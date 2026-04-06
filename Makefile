# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall

# ==========================================
# make compile FILE=<filename.cpp>
# ==========================================
# This target should compile your files with the provided 
# main.cpp. The main.cpp will always #include "Processor.h" 
# and will have its own main() function.
compile:
	@echo "Compiling simulator:"
	$(CXX) $(CXXFLAGS) $(FILE) -o main
	@echo "Build successful, 'main' created."

# ==========================================
# make run FILE=<filename.s>
# ==========================================
# Runs the simulator on the provided assembly file.
# Optional argument: CYCLES=<N> to pass "-cycles N".
run:
	@if [ -z "$(FILE)" ]; then \
		echo "Usage: make run FILE=<filename.s> [CYCLES=<N>]"; \
		exit 1; \
	fi
	@if [ ! -f "$(FILE)" ]; then \
		echo "Error: input file '$(FILE)' not found."; \
		exit 1; \
	fi
	@if [ ! -x ./main ]; then \
		echo "Error: executable 'main' not found. Run 'make compile FILE=main.cpp' first."; \
		exit 1; \
	fi
	@echo "Running $(FILE)..."
	@if [ -n "$(CYCLES)" ]; then \
		./main "$(FILE)" -cycles "$(CYCLES)"; \
	else \
		./main "$(FILE)"; \
	fi