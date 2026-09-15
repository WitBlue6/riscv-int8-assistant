PYTHON ?= python3
RVCC ?= /opt/homebrew/opt/llvm/bin/clang
LLD ?= $(shell command -v ld.lld)
FLAGS = --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32 -O2 -ffreestanding -fno-builtin -msmall-data-limit=0 -mno-relax -nostdlib -Wall -Wextra
.PHONY: build train demo test serve
train:
	OPENBLAS_NUM_THREADS=1 $(PYTHON) train.py
weights.h weights.json: train.py model.py corpus.json
	$(MAKE) train
build/transformer.elf: main.c weights.h ../firmware/start.S ../firmware/link.ld
	mkdir -p build
	$(RVCC) $(FLAGS) --ld-path=$(LLD) -Wl,-T,../firmware/link.ld -Wl,--no-relax ../firmware/start.S main.c -o $@
build/software.elf: main.c weights.h ../firmware/start.S ../firmware/link.ld
	mkdir -p build
	$(RVCC) $(FLAGS) -DUSE_ACCEL=0 --ld-path=$(LLD) -Wl,-T,../firmware/link.ld -Wl,--no-relax ../firmware/start.S main.c -o $@
build/fast.elf: main.c weights.h ../firmware/start.S ../firmware/link.ld
	mkdir -p build
	$(RVCC) $(FLAGS) -DVERIFY_MAC=0 --ld-path=$(LLD) -Wl,-T,../firmware/link.ld -Wl,--no-relax ../firmware/start.S main.c -o $@
build/limit.elf: main.c weights.h ../firmware/start.S ../firmware/link.ld
	mkdir -p build
	$(RVCC) $(FLAGS) -DNEW_TOKENS=1 --ld-path=$(LLD) -Wl,-T,../firmware/link.ld -Wl,--no-relax ../firmware/start.S main.c -o $@
build/%.hex: build/%.elf ../scripts/elf_to_hex.py
	$(PYTHON) ../scripts/elf_to_hex.py $< $@
build: build/transformer.hex build/software.hex build/fast.hex
	$(MAKE) -C .. build/obj/Vsoc
demo: build
	../build/obj/Vsoc +firmware=$(CURDIR)/build/transformer.hex --text '你好：'
test: build build/limit.hex
	$(PYTHON) test.py
serve: build
	$(PYTHON) server.py
