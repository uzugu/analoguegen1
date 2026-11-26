# Top-level Makefile for analoguegen1
# Runs inside Docker (recommended) or any Linux/MSYS2 host with riscv64-unknown-elf-gcc + SDL2.

DOCKER_IMAGE ?= analoguegen1-dev

.PHONY: docker-image pocket pc clean help

help:
	@echo "Targets:"
	@echo "  make docker-image   Build the Docker toolchain image ($(DOCKER_IMAGE))"
	@echo "  make pocket         Build Analogue Pocket RISC-V ELF (build/app.elf)"
	@echo "  make pc             Build Linux/SDL2 desktop test binary (src/app_pc)"
	@echo "  make clean          Remove build artifacts"

docker-image:
	docker build -t $(DOCKER_IMAGE) .

pocket:
	$(MAKE) -C src pocket

pc:
	$(MAKE) -C src pc

clean:
	$(MAKE) -C src clean
	rm -rf build
