#!/bin/bash
set -e

# Load defaults
source "$(dirname "$0")/env.sh"

# Args:
#   $1 -> KERNEL_COMPILER (optional)
#   $2 -> KERNEL_SRC (optional)
#   $3 -> ARCH (optional)
if [ -n "$1" ]; then
    KERNEL_COMPILER="$1"
    echo "Info: Using custom KERNEL_COMPILER: $KERNEL_COMPILER"
fi

if [ -n "$2" ]; then
    KERNEL_SRC="$2"
    echo "Info: Using custom KERNEL_SRC: $KERNEL_SRC"
fi

if [ -n "$3" ]; then
    ARCH="$3"
    echo "Info: Using custom ARCH: $ARCH"
fi

# Build the full CROSS_COMPILE with CCACHE if needed
if [ -n "$CCACHE" ]; then
    CROSS_COMPILE="$CCACHE $KERNEL_COMPILER"
else
    CROSS_COMPILE="$KERNEL_COMPILER"
fi

WORK_DIR="$(dirname "$(readlink -f "$0")")"
SOURCE_DIR="$WORK_DIR"
OUTPUT_DIR="$WORK_DIR/output"
MODULES_DIR="$OUTPUT_DIR/_modules"
DEB_BUILD_DIR="$OUTPUT_DIR/package"

if [ ! -d "$KERNEL_SRC" ]; then
    echo "Error: KERNEL_SRC not found: $KERNEL_SRC"
    exit 1
fi

if [ ! -f "$SOURCE_DIR/f81601a.c" ]; then
    echo "Error: f81601a.c not found in $SOURCE_DIR"
    exit 1
fi

echo "=== Environment Info ==="
echo "Work Dir:       $WORK_DIR"
echo "Kernel Src:     $KERNEL_SRC"
echo "Arch:           $ARCH"
echo "Compiler:       ${CROSS_COMPILE}gcc"
echo "Package:        $PACKAGE_NAME"
echo "Version:        $DEB_VERSION"
echo "Install ModDir: $INSTALL_MOD_DIR"
echo "========================"

echo "[1/4] Cleanup..."
rm -rf "$OUTPUT_DIR"
make -C "$KERNEL_SRC" M="$SOURCE_DIR" clean >/dev/null 2>&1 || true

JOBS="$(nproc)"

echo "[2/4] Build kernel module..."
make -C "$KERNEL_SRC" \
    M="$SOURCE_DIR" \
    ARCH="$ARCH" \
    CROSS_COMPILE="$KERNEL_COMPILER" \
    modules -j"$JOBS"

echo "[3/4] Stage files..."
make -C "$KERNEL_SRC" \
    M="$SOURCE_DIR" \
    ARCH="$ARCH" \
    CROSS_COMPILE="$KERNEL_COMPILER" \
    INSTALL_MOD_PATH="$MODULES_DIR" \
    INSTALL_MOD_DIR="$INSTALL_MOD_DIR" \
    DEPMOD=true \
    modules_install

# Remove generated module index files to avoid target conflicts.
find "$MODULES_DIR" -name "modules.alias" -type f -delete
find "$MODULES_DIR" -name "modules.alias.bin" -type f -delete
find "$MODULES_DIR" -name "modules.dep" -type f -delete
find "$MODULES_DIR" -name "modules.dep.bin" -type f -delete
find "$MODULES_DIR" -name "modules.softdep" -type f -delete
find "$MODULES_DIR" -name "modules.symbols" -type f -delete
find "$MODULES_DIR" -name "modules.symbols.bin" -type f -delete
find "$MODULES_DIR" -name "modules.builtin.bin" -type f -delete
find "$MODULES_DIR" -name "modules.builtin.alias.bin" -type f -delete
find "$MODULES_DIR" -name "modules.devname" -type f -delete

mkdir -p "$DEB_BUILD_DIR"
cp -r "$MODULES_DIR"/* "$DEB_BUILD_DIR/"
mkdir -p "$DEB_BUILD_DIR/DEBIAN"
cp -r "$WORK_DIR/debian/DEBIAN/"* "$DEB_BUILD_DIR/DEBIAN/"
chmod 755 "$DEB_BUILD_DIR/DEBIAN/post"* "$DEB_BUILD_DIR/DEBIAN/pre"* 2>/dev/null || true

BUILD_KERNEL_VER=""
if [ -d "$MODULES_DIR/lib/modules" ]; then
    BUILD_KERNEL_VER="$(ls "$MODULES_DIR/lib/modules" | head -n 1)"
fi

KERNEL_SUFFIX=""
if [ -n "$BUILD_KERNEL_VER" ]; then
    KERNEL_SUFFIX="_${BUILD_KERNEL_VER}"
    echo "Detected built kernel version: $BUILD_KERNEL_VER"
fi

if [ ! -f "$DEB_BUILD_DIR/DEBIAN/control" ]; then
    cat <<EOF > "$DEB_BUILD_DIR/DEBIAN/control"
Package: ${PACKAGE_NAME}
Version: ${DEB_VERSION}${KERNEL_SUFFIX//_/-}
Architecture: ${ARCH}
Section: kernel
Priority: optional
Maintainer: Flora <2321901849@qq.com>
Depends: kmod
Description: Fintek F81601A PCIe CAN kernel module
 Built from local source and packaged for deployment.
EOF
fi

echo "[4/4] Build DEB..."
DEB_NAME="${PACKAGE_NAME}_${DEB_VERSION}${KERNEL_SUFFIX}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$DEB_BUILD_DIR" "$OUTPUT_DIR/${DEB_NAME}"

echo "=== Build Success! ==="
echo "Package: $OUTPUT_DIR/${DEB_NAME}"
