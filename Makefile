CC ?= cc
CFLAGS ?= -O3 -DNDEBUG -std=c11 -Wall -Wextra -Wpedantic

.PHONY: all clean test corpus research-corpus benchmark benchmark-screen benchmark-validate

CACHE ?= results/exp-0004
LABEL ?= staged
CODECS ?=

all: bin/bite

bin/bite: src/bite.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $< -o $@

corpus:
	python3 scripts/generate_corpus.py
	./scripts/fetch_canterbury.sh

research-corpus:
	./scripts/fetch_research_corpora.sh

test: bin/bite corpus
	python3 scripts/test_roundtrip.py

benchmark: bin/bite corpus
	python3 scripts/benchmark.py --stage audit --repeats 3

benchmark-screen: bin/bite corpus
	python3 scripts/benchmark.py --stage screen --codecs $(CODECS) --baseline-cache $(CACHE) --label $(LABEL)-screen

benchmark-validate: bin/bite corpus
	python3 scripts/benchmark.py --stage validate --codecs $(CODECS) --baseline-cache $(CACHE) --label $(LABEL)-validate

clean:
	rm -rf bin build
