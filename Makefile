CC ?= cc
CFLAGS ?= -O3 -DNDEBUG -std=c11 -Wall -Wextra -Wpedantic -pthread

.PHONY: all clean test test-bwt corpus research-corpus benchmark benchmark-screen benchmark-validate

CACHE ?= results/exp-0004
LABEL ?= staged
CODECS ?=

all: bin/bite bin/bite-bench

bin/bite: src/bite.c src/bite_bwt.h
	@mkdir -p bin
	$(CC) $(CFLAGS) $< -o $@

bin/bite-bench: src/bite_bench.c src/bite.c src/bite_bwt.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -Wno-unused-function src/bite_bench.c -o $@

corpus:
	python3 scripts/generate_corpus.py
	./scripts/fetch_canterbury.sh

research-corpus:
	./scripts/fetch_research_corpora.sh

test: bin/bite corpus
	python3 scripts/test_roundtrip.py

test-bwt:
	python3 scripts/test_bwt.py

benchmark: bin/bite corpus
	python3 scripts/benchmark.py --stage audit --repeats 3

benchmark-screen: bin/bite corpus
	python3 scripts/benchmark.py --stage screen --codecs $(CODECS) --baseline-cache $(CACHE) --label $(LABEL)-screen

benchmark-validate: bin/bite corpus
	python3 scripts/benchmark.py --stage validate --codecs $(CODECS) --baseline-cache $(CACHE) --label $(LABEL)-validate

clean:
	rm -rf bin build
