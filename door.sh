#!/bin/bash
# ==============================================================================
# door.sh — MME Simulator Automation Script
# Purpose: One-step soft compilation and execution of the MME binary.
# Focus: MME Simulator (EPC Logic)
# ==============================================================================

# Exit on any command failure
set -e

PROJECT_ROOT="/Users/abhichauhan/Desktop/cpp-interview-prep/mme-simulator"
BUILD_DIR="$PROJECT_ROOT/build"

echo -e "\033[1;35m[SYSTEM] Starting MME Build & Run Sequence...\033[0m"

# 1. Compilation Step
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo -e "\033[1;34m[SYSTEM] Configuring and Compiling All Targets (MME + IMS)...\033[0m"
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(sysctl -n hw.ncpu || nproc || echo 2)

# 2. Cleanup old capture to ensure fresh analysis
rm -f "$BUILD_DIR/mme_capture.pcap"

echo -e "\033[1;32m[SYSTEM] Build Successful.\033[0m"
echo -e "\033[1;33m[INFO] PCAP will be captured at: $BUILD_DIR/mme_capture.pcap\033[0m"
echo -e "\033[1;33m[INFO] Launching MME Simulator... (Type 'QUIT' to save PCAP)\033[0m"
echo "----------------------------------------------------------------"
./mme_sim