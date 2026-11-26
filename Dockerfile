FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc-riscv64-unknown-elf \
    binutils-riscv64-unknown-elf \
    libsdl2-dev \
    pkg-config \
    python3 \
    git \
    make \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ENV OF_SDK_IN_CONTAINER=1
ENV USE_SDK_CONTAINER=0

WORKDIR /workspace
