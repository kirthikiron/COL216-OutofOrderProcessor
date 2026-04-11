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
	@echo "No preprocessing required. C++ parser handles labels natively."