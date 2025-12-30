#!/bin/bash
#
# Setup BTRFS filesystem for BURST integration tests
#
# Creates an 8 GiB loop device with BTRFS if not already available.
# Requires sudo for loop device creation and mounting.
#
# Usage:
#   source setup_btrfs.sh    # Sets BTRFS_MOUNT_DIR
#
# Environment Variables Set:
#   BTRFS_MOUNT_DIR           - Path to BTRFS mount (tests/tmp/btrfs)
#   BTRFS_LOOP_DEVICE         - Loop device path (e.g., /dev/loop0) if created
#   BTRFS_LOOP_IMAGE          - Path to loop device image file
#
# Note: BTRFS is kept mounted between test runs to reduce sudo prompts.
# To unmount manually: sudo umount tests/tmp/btrfs && sudo losetup -d /dev/loopX
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TMP_DIR="$PROJECT_ROOT/tests/tmp"
BTRFS_MOUNT_DIR="$TMP_DIR/btrfs"
BTRFS_LOOP_IMAGE="$TMP_DIR/loop_devices/btrfs.img"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Check if BTRFS filesystem already exists at expected location
check_existing_btrfs() {
    if [ -d "$BTRFS_MOUNT_DIR" ]; then
        # Check if it's mounted
        if mountpoint -q "$BTRFS_MOUNT_DIR" 2>/dev/null; then
            # Check if it's actually BTRFS
            local fstype
            fstype=$(stat -f -c %T "$BTRFS_MOUNT_DIR" 2>/dev/null || echo "unknown")
            if [ "$fstype" = "btrfs" ]; then
                echo -e "${GREEN}✓${NC} Using existing BTRFS filesystem at $BTRFS_MOUNT_DIR"
                return 0
            else
                echo -e "${YELLOW}Warning:${NC} $BTRFS_MOUNT_DIR is mounted but not BTRFS (type: $fstype)"
                return 1
            fi
        fi
    fi
    return 1
}

# Check if sudo is available
check_sudo() {
    if ! sudo -n true 2>/dev/null; then
        echo -e "${RED}Error: sudo access required for BTRFS setup${NC}"
        echo ""
        echo "This test requires a BTRFS filesystem for BTRFS_IOC_ENCODED_WRITE testing."
        echo "To create a loop device with BTRFS, sudo access is needed."
        echo ""
        echo "Options:"
        echo "  1. Run 'sudo -v' before running tests (caches credentials)"
        echo "  2. Pre-create BTRFS mount at: $BTRFS_MOUNT_DIR"
        echo ""
        return 1
    fi
    return 0
}

# Create loop device with BTRFS
create_btrfs_loop() {
    echo "Creating 8 GiB loop device with BTRFS..."

    # Create directories
    mkdir -p "$TMP_DIR/loop_devices"
    mkdir -p "$BTRFS_MOUNT_DIR"

    # Create 8 GiB sparse file
    echo "  Creating sparse image file..."
    dd if=/dev/zero of="$BTRFS_LOOP_IMAGE" bs=1M count=0 seek=8192 2>/dev/null

    # Setup loop device
    echo "  Setting up loop device..."
    BTRFS_LOOP_DEVICE=$(sudo losetup -f --show "$BTRFS_LOOP_IMAGE")
    echo -e "  ${GREEN}✓${NC} Loop device: $BTRFS_LOOP_DEVICE"

    # Create BTRFS filesystem
    echo "  Creating BTRFS filesystem..."
    sudo mkfs.btrfs -f -L "burst-test" "$BTRFS_LOOP_DEVICE" >/dev/null 2>&1

    # Mount filesystem
    echo "  Mounting BTRFS..."
    sudo mount -o compress=zstd "$BTRFS_LOOP_DEVICE" "$BTRFS_MOUNT_DIR"

    # Change ownership to current user
    sudo chown -R "$(id -u):$(id -g)" "$BTRFS_MOUNT_DIR"

    echo -e "${GREEN}✓${NC} BTRFS filesystem ready at $BTRFS_MOUNT_DIR"
}

# Main setup logic
setup_btrfs_main() {
    echo -e "${BLUE}=== BTRFS Setup ===${NC}"

    # Check for existing BTRFS
    if check_existing_btrfs; then
        return 0
    fi

    # Need to create new BTRFS - check sudo
    if ! check_sudo; then
        return 1
    fi

    # Create BTRFS loop device
    if ! create_btrfs_loop; then
        echo -e "${RED}Error: Failed to create BTRFS filesystem${NC}"
        return 1
    fi

    return 0
}

# Run setup
if ! setup_btrfs_main; then
    echo -e "${RED}BTRFS setup failed${NC}"
    exit 1
fi

# Export variables for use by test scripts
export BTRFS_MOUNT_DIR
export BTRFS_LOOP_DEVICE
export BTRFS_LOOP_IMAGE

echo ""
