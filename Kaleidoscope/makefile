CC=clang++

all : toy

toy : toy.cpp
	$(CC) -g -O3 toy.cpp `llvm-config --cxxflags --ldflags --system-libs --libs core mcjit native` -o toy

clean :
	rm toy
