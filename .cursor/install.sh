#!/usr/bin/env bash
# Cloud Agent install: prepare the toolchain and Raspberry Pi Pico SDK needed to
# build the USBPods Max firmware and run the host GCC tests. Idempotent: safe to
# re-run and safe to run from a warm snapshot.
set -euo pipefail

PICO_SDK_VERSION="2.1.1"
PICO_SDK_PATH="${PICO_SDK_PATH:-$HOME/pico-sdk}"
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "==> Installing system packages (ARM toolchain, ninja, libusb)"
export DEBIAN_FRONTEND=noninteractive
sudo apt-get update -qq
# build-essential / g++ are already present on the base image; the ARM bare-metal
# toolchain, ninja and libusb (for the SDK-built picotool) are what we add.
sudo apt-get install -y -qq \
  gcc-arm-none-eabi \
  ninja-build \
  cmake \
  libusb-1.0-0-dev \
  pkg-config \
  build-essential

echo "==> Cloning Raspberry Pi Pico SDK ${PICO_SDK_VERSION} (with submodules)"
if [ ! -d "$PICO_SDK_PATH/.git" ]; then
  git clone --branch "$PICO_SDK_VERSION" --depth 1 \
    https://github.com/raspberrypi/pico-sdk.git "$PICO_SDK_PATH"
fi
git -C "$PICO_SDK_PATH" submodule update --init --depth 1 \
  lib/tinyusb lib/btstack lib/cyw43-driver lib/lwip lib/mbedtls

echo "==> Applying required TinyUSB 0.18 RP2350 panic patch"
PATCH="$REPO_DIR/patches/tinyusb-0.18-rp2350-panic-fix.patch"
TINYUSB_DIR="$PICO_SDK_PATH/lib/tinyusb"
if git -C "$TINYUSB_DIR" apply --reverse --check "$PATCH" >/dev/null 2>&1; then
  echo "    patch already applied, skipping"
elif git -C "$TINYUSB_DIR" apply --check "$PATCH" >/dev/null 2>&1; then
  git -C "$TINYUSB_DIR" apply "$PATCH"
  echo "    patch applied"
else
  echo "    WARNING: patch neither applies cleanly nor is already applied" >&2
fi

echo "==> Configuring shell environment (PICO_SDK_PATH, host compiler)"
# The base image maps cc/c++ to clang, whose libstdc++ auto-detection is broken
# here and breaks the SDK's host tools (picotool/pioasm). Pin the host compiler
# to gcc/g++ and export the SDK path for interactive agent shells. Guarded so
# re-running install does not append duplicates.
BASHRC="$HOME/.bashrc"
add_line() { grep -qxF "$1" "$BASHRC" 2>/dev/null || echo "$1" >> "$BASHRC"; }
touch "$BASHRC"
add_line "export PICO_SDK_PATH=\"$PICO_SDK_PATH\""
add_line "export CC=gcc"
add_line "export CXX=g++"

echo "==> install complete"
echo "    PICO_SDK_PATH=$PICO_SDK_PATH"
arm-none-eabi-gcc --version | head -1
ninja --version
