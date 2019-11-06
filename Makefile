ARCH ?= native
OBJS = build/vcf_line_splitter.o
CXXFLAGS = -Wall -std=c++14 -pthread -O3 -DNDEBUG -march=$(ARCH)
LDFLAGS = -ljemalloc -lgflags -lhts -lpthread -march=$(ARCH)

all:
	mkdir -p build
	$(MAKE) build/vcf_line_splitter

build/vcf_line_splitter: $(OBJS)
	g++ -o $@ $(OBJS) $(LDFLAGS)

build/%.o: %.cc
	g++ -c -o $@ $(CXXFLAGS) $^

clean:
	rm -rf build

.PHONY: all test clean
