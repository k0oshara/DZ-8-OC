#!/bin/bash
set -e

IMAGE="ext2.img"
MOUNT_DIR="mnt"
LOOP_DEVICE=""
TEST_FILE_SPARSE="sparse_file.bin"
TEST_FILE_LARGE="large_file.bin"
TEST_DIRS=("dir1" "dir2" "dir3")
UTILITY="./ext2reader"
CHECKSUM_FILE="/tmp/ext2_checksums.txt"

if [ ! -x "$UTILITY" ]; then
    echo "Error: Utility $UTILITY not found or not executable"
    echo "Compile program with: gcc -Wall -Wextra -pedantic -o ext2reader ext2reader.c"
    exit 1
fi

cleanup() {
    echo "Cleaning up..."
    sudo umount "$MOUNT_DIR" 2>/dev/null || true
    sudo losetup -d "$LOOP_DEVICE" 2>/dev/null || true
    rm -rf "$MOUNT_DIR"
    rm -f "$IMAGE"
    rm -f "$CHECKSUM_FILE"
}

trap cleanup EXIT

echo "1. Creating 1GB image..."
dd if=/dev/zero of="$IMAGE" bs=1M count=1024 status=progress
sync

echo "2. Creating ext2 filesystem..."
mkfs.ext2 -q -F -O ^has_journal -b 4096 -N 10000 "$IMAGE" 2>/dev/null

mkdir -p "$MOUNT_DIR"

echo "3. Mounting image..."
LOOP_DEVICE=$(sudo losetup -f --show "$IMAGE")
sudo chown $USER:$USER "$LOOP_DEVICE"
sudo mount -t ext2 "$LOOP_DEVICE" "$MOUNT_DIR"
sudo chown $USER:$USER "$MOUNT_DIR"

echo "4. Generating test data..."
echo "Test data for regular file" | sudo tee "$MOUNT_DIR/normal_file.txt" >/dev/null
sudo truncate -s 100M "$MOUNT_DIR/$TEST_FILE_SPARSE"
sudo dd if=/dev/urandom of="$MOUNT_DIR/$TEST_FILE_LARGE" bs=4K count=1280 status=progress

for dir in "${TEST_DIRS[@]}"; do
    sudo mkdir "$MOUNT_DIR/$dir"
done

pushd "$MOUNT_DIR" >/dev/null
    sudo sha512sum "normal_file.txt" > "$CHECKSUM_FILE"
    sudo sha512sum "$TEST_FILE_SPARSE" >> "$CHECKSUM_FILE"
    sudo sha512sum "$TEST_FILE_LARGE" >> "$CHECKSUM_FILE"
    sudo chown $USER:$USER "$CHECKSUM_FILE"
popd >/dev/null

INODE_NORMAL=$(sudo stat -c '%i' "$MOUNT_DIR/normal_file.txt")
INODE_SPARSE=$(sudo stat -c '%i' "$MOUNT_DIR/$TEST_FILE_SPARSE")
INODE_LARGE=$(sudo stat -c '%i' "$MOUNT_DIR/$TEST_FILE_LARGE")
INODE_DIRS=()
for dir in "${TEST_DIRS[@]}"; do
    INODE_DIRS+=($(sudo stat -c '%i' "$MOUNT_DIR/$dir"))
done

echo "5. Unmounting..."
sudo umount "$MOUNT_DIR"
sudo losetup -d "$LOOP_DEVICE"

echo "6. Testing via image file..."
echo "Testing regular file (inode $INODE_NORMAL):"
$UTILITY "$IMAGE" "$INODE_NORMAL" | sha512sum -c <(awk '{print $1 "  -"}' <(head -1 "$CHECKSUM_FILE"))

echo "Testing sparse file (inode $INODE_SPARSE):"
$UTILITY "$IMAGE" "$INODE_SPARSE" | sha512sum -c <(awk '{print $1 "  -"}' <(sed -n 2p "$CHECKSUM_FILE"))

echo "Testing large file (inode $INODE_LARGE):"
$UTILITY "$IMAGE" "$INODE_LARGE" | sha512sum -c <(awk '{print $1 "  -"}' <(sed -n 3p "$CHECKSUM_FILE"))

echo "7. Testing via block device..."
LOOP_DEVICE=$(sudo losetup -f --show "$IMAGE")
sudo chown $USER:$USER "$LOOP_DEVICE"
echo "Using device: $LOOP_DEVICE"

echo "Block device info:"
lsblk -o NAME,SIZE,FSTYPE "$LOOP_DEVICE"

echo "Block device test:"
$UTILITY "$LOOP_DEVICE" "$INODE_NORMAL" | sha512sum -c <(awk '{print $1 "  -"}' <(head -1 "$CHECKSUM_FILE"))

echo "8. Directory structure tests:"
for ((i=0; i<${#TEST_DIRS[@]}; i++)); do
    echo "Directory ${TEST_DIRS[$i]} (inode ${INODE_DIRS[$i]}):"
    $UTILITY "$IMAGE" "${INODE_DIRS[$i]}" | hexdump -C | head -n 20
    echo "----------------------------------------"
done

echo "All tests completed successfully!"
