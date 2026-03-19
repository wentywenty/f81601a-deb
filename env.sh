#!/bin/bash

# Toolchain prefix (can be overridden externally)
# Example: /opt/toolchains/aarch64-none-linux-gnu-
export KERNEL_COMPILER="${KERNEL_COMPILER:-/opt/atom01/orangepi-build/toolchains/gcc-arm-11.2-2022.02-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu-}"

# Build architecture
export ARCH="${ARCH:-arm64}"

# Kernel source tree (must be configured/built)
export KERNEL_SRC="${KERNEL_SRC:-/opt/atom01/orangepi-build/kernel/orange-pi-6.1-rk35xx}"

# Optional ccache wrapper command (e.g. "ccache")
export CCACHE="${CCACHE:-}"

# Package metadata
export PACKAGE_NAME="${PACKAGE_NAME:-f81601a-can}"
export DEB_VERSION="${DEB_VERSION:-1.03.20250515}"

# Kernel module install subdir under /lib/modules/<ver>/
export INSTALL_MOD_DIR="${INSTALL_MOD_DIR:-updates}"
