CC ?= cc
CFLAGS ?= -O3 -DNDEBUG -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all clean test corpus benchmark

all: bin/bite

bin/bite: src/bite.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $< -o $@

corpus:
	python3 scripts/generate_corpus.py
	./scripts/fetch_canterbury.sh

test: bin/bite corpus
	python3 scripts/test_roundtrip.py

benchmark: bin/bite corpus
	python3 scripts/benchmark.py --repeats 3

clean:
	rm -rf bin build
