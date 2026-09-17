.DEFAULT_GOAL := build
PYTHON ?= python3
RVCC ?= /opt/homebrew/opt/llvm/bin/clang
LLD ?= $(shell command -v ld.lld)
FLAGS = --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32 -O2 -ffreestanding -fno-builtin -msmall-data-limit=0 -mno-relax -nostdlib -Wall -Wextra
VFLAGS = --build -j 4 --cc --exe -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-PINMISSING --timescale 1ns/1ps
.PHONY: build train demo test unit serve
train:
	OPENBLAS_NUM_THREADS=1 $(PYTHON) train.py
weights.h weights.json external.hex: train.py model.py corpus.json
	$(MAKE) train
build/fast.elf: main.c weights.h firmware/start.S firmware/link.ld
	mkdir -p build
	$(RVCC) $(FLAGS) --ld-path=$(LLD) -Wl,-T,firmware/link.ld -Wl,--no-relax firmware/start.S main.c -o $@
build/checked.elf: main.c weights.h firmware/start.S firmware/link.ld
	mkdir -p build
	$(RVCC) $(FLAGS) -DVERIFY_MAC=1 --ld-path=$(LLD) -Wl,-T,firmware/link.ld -Wl,--no-relax firmware/start.S main.c -o $@
build/cpu_attention.elf: main.c weights.h firmware/start.S firmware/link.ld
	mkdir -p build
	$(RVCC) $(FLAGS) -DUSE_ATTENTION=0 --ld-path=$(LLD) -Wl,-T,firmware/link.ld -Wl,--no-relax firmware/start.S main.c -o $@
build/cpu.elf: main.c weights.h firmware/start.S firmware/link.ld
	mkdir -p build
	$(RVCC) $(FLAGS) -DUSE_ATTENTION=0 -DUSE_ACCEL=0 --ld-path=$(LLD) -Wl,-T,firmware/link.ld -Wl,--no-relax firmware/start.S main.c -o $@
build/%.hex: build/%.elf scripts/elf_to_hex.py
	$(PYTHON) scripts/elf_to_hex.py $< $@
build/obj/Vsoc: soc.sv tiled_fc.sv attention.sv main.cpp vendor/picorv32/picorv32.v
	verilator $(VFLAGS) --trace --top-module soc --Mdir build/obj soc.sv tiled_fc.sv attention.sv vendor/picorv32/picorv32.v $(CURDIR)/main.cpp
build: build/fast.hex build/checked.hex build/cpu_attention.hex build/cpu.hex external.hex build/obj/Vsoc
demo: build
	build/obj/Vsoc +firmware=$(CURDIR)/build/fast.hex +weights=$(CURDIR)/external.hex +ext_latency=3 --text '你好：'
build/attention/Vattention: attention.sv attention_test.cpp
	verilator $(VFLAGS) --top-module attention --Mdir build/attention attention.sv $(CURDIR)/attention_test.cpp
build/fc/Vtiled_fc: tiled_fc.sv fc_test.cpp
	verilator $(VFLAGS) --top-module tiled_fc --Mdir build/fc tiled_fc.sv $(CURDIR)/fc_test.cpp
unit: build/attention/Vattention build/fc/Vtiled_fc
	build/attention/Vattention
	build/fc/Vtiled_fc
test: build unit
	$(PYTHON) test.py

serve: build
	$(PYTHON) server.py
