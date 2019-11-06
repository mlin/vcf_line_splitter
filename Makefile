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

test: all
	docker build -t vcf_line_splitter .
	miniwdl run --verbose test.wdl

clean:
	rm -rf build

.PHONY: all test clean
