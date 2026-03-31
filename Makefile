# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall

# Local defaults (autograder can override FILE)
FILE ?= main.cpp
ASM ?= tests/test1.s

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
# No preprocessing step is required for this parser.
# Keep this target to satisfy the autograder interface.
run:
	@echo "No preprocessing required for $(FILE)."

# Convenience local runner
run-sim: compile
	./main $(ASM)