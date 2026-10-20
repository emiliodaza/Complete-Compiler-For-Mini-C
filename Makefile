LLVM_CONFIG ?= $(shell command -v llvm-config 2>/dev/null || if [ -x /opt/homebrew/opt/llvm/bin/llvm-config ]; then echo /opt/homebrew/opt/llvm/bin/llvm-config; elif [ -x /usr/local/opt/llvm/bin/llvm-config ]; then echo /usr/local/opt/llvm/bin/llvm-config; fi)
BISON ?= $(shell if [ -x /opt/homebrew/opt/bison/bin/bison ]; then echo /opt/homebrew/opt/bison/bin/bison; else command -v bison; fi)
FLEX ?= flex
CXX = c++
PYTHON ?= $(shell if [ -x .venv/bin/python ]; then echo .venv/bin/python; else command -v python3; fi)
CPPFLAGS += -I. -Ibuild
CXXFLAGS += -O2 -g -Wall -Wextra
SOURCES = main_function.cpp IR_Builder.cpp local_and_global.cpp assembly_code_generator.cpp
OBJECTS = $(SOURCES:%.cpp=build/%.o) build/ast.o build/parser_and_analyzer.tab.o build/lex.yy.o

.PHONY: all check-deps clean test test-native demo
all: minic

check-deps:
	@test -n "$(LLVM_CONFIG)" || { echo 'LLVM development tools missing. Install LLVM or set LLVM_CONFIG=/path/to/llvm-config.' >&2; exit 1; }
	@$(LLVM_CONFIG) --version >/dev/null
	@$(BISON) --version >/dev/null
	@$(FLEX) --version >/dev/null

build:
	mkdir -p build

build/parser_and_analyzer.tab.cpp: parser_and_analyzer.y ast.h | build check-deps
	$(BISON) --defines=build/parser_and_analyzer.tab.h -o $@ $<

build/parser_and_analyzer.tab.h: build/parser_and_analyzer.tab.cpp
	@test -f $@

build/lex.yy.cpp: parser_and_analyzer.l build/parser_and_analyzer.tab.h | build
	$(FLEX) -o $@ $<

build/%.o: %.cpp ast.h | build check-deps
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $$($(LLVM_CONFIG) --cxxflags) -c $< -o $@

build/ast.o: ast.c ast.h | build check-deps
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $$($(LLVM_CONFIG) --cxxflags) -x c++ -c $< -o $@

build/parser_and_analyzer.tab.o: build/parser_and_analyzer.tab.cpp ast.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $$($(LLVM_CONFIG) --cxxflags) -c $< -o $@

build/lex.yy.o: build/lex.yy.cpp ast.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $$($(LLVM_CONFIG) --cxxflags) -Wno-unused-function -c $< -o $@

minic: $(OBJECTS)
	$(CXX) $(OBJECTS) $$($(LLVM_CONFIG) --ldflags --libs core analysis native --system-libs) -o $@

test: minic
	$(PYTHON) tests/test_compiler.py

test-native: minic
	$(PYTHON) tests/test_compiler.py --native

demo: minic
	$(PYTHON) tools/run.py examples/factorial.c 6

clean:
	$(RM) -r build minic
