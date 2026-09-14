PYTHON ?= python3
RVCC ?= $(shell command -v /opt/homebrew/opt/llvm/bin/clang 2>/dev/null || command -v clang)
LLD ?= $(shell command -v ld.lld)
CFLAGS_RV = --target=riscv32-unknown-elf -march=rv32im -mabi=ilp32 -O2 -ffreestanding -fno-builtin -msmall-data-limit=0 -mno-relax -nostdlib -Wall -Wextra
VFLAGS = --build -j 4 --cc --exe -Wno-fatal -Wno-WIDTHEXPAND -Wno-WIDTHTRUNC -Wno-PINMISSING --timescale 1ns/1ps

.PHONY: all train build test serve demo unit
all: build
train:
	OPENBLAS_NUM_THREADS=1 $(PYTHON) scripts/train.py
firmware/model_weights.h model/weights.json: scripts/train.py scripts/model_common.py model/dataset.json
	$(MAKE) train
build/firmware.elf: firmware/main.c firmware/start.S firmware/link.ld firmware/model_weights.h
	mkdir -p build
	$(RVCC) $(CFLAGS_RV) --ld-path=$(LLD) -Wl,-T,firmware/link.ld -Wl,--no-relax firmware/start.S firmware/main.c -o $@
build/baseline.elf: firmware/main.c firmware/start.S firmware/link.ld firmware/model_weights.h
	mkdir -p build
	$(RVCC) $(CFLAGS_RV) -DACCEL_WORD_COPY=0 --ld-path=$(LLD) -Wl,-T,firmware/link.ld -Wl,--no-relax firmware/start.S firmware/main.c -o $@
build/baseline.hex: build/baseline.elf scripts/elf_to_hex.py
	$(PYTHON) scripts/elf_to_hex.py $< $@
build/firmware.hex: build/firmware.elf scripts/elf_to_hex.py
	$(PYTHON) scripts/elf_to_hex.py $< $@
build/obj/Vsoc: rtl/soc.sv rtl/int8_accel.sv vendor/picorv32/picorv32.v sim/main.cpp
	mkdir -p build
	verilator $(VFLAGS) --trace --top-module soc --Mdir build/obj rtl/soc.sv rtl/int8_accel.sv vendor/picorv32/picorv32.v $(CURDIR)/sim/main.cpp
build: build/firmware.hex build/obj/Vsoc
build/unit/Vint8_accel: rtl/int8_accel.sv tests/accel_test.cpp
	mkdir -p build
	verilator $(VFLAGS) --top-module int8_accel --Mdir build/unit rtl/int8_accel.sv $(CURDIR)/tests/accel_test.cpp
unit: build/unit/Vint8_accel
	build/unit/Vint8_accel
test: build unit build/baseline.hex
	$(PYTHON) tests/system_test.py
demo: build
	build/obj/Vsoc +firmware=build/firmware.hex --text '帮我算一下12加30'
serve: build
	$(PYTHON) scripts/server.py
.PHONY: ui-test trace
ui-test: build
	node tests/browser_smoke.cjs
trace: build
	build/obj/Vsoc +firmware=build/firmware.hex --text '你好' --trace build/inference.vcd
