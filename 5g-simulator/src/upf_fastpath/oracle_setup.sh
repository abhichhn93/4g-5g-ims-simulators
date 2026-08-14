#!/usr/bin/env bash
# =============================================================================
# oracle_setup.sh — DPDK setup for Oracle Cloud Free Tier (A1 / AMD VM)
#
# ORACLE CLOUD FREE TIER (always-free):
#   A1 Flex (ARM): 4 OCPUs, 24GB RAM, 200GB boot disk
#   AMD: 1/8 OCPU, 1GB RAM (too small for DPDK — use A1)
#
# WHAT THIS SCRIPT DOES:
#   1. Install DPDK dev libraries + build tools
#   2. Mount 2MB hugepages (required for DPDK memory)
#   3. Build the UPF fast-path with real DPDK
#   4. Run with net_tap virtual device (no physical NIC binding needed)
#
# USAGE:
#   ssh opc@<your-oracle-ip>
#   git clone https://github.com/abhichhn93/4g-5g-ims-simulators
#   cd 4g-5g-ims-simulators/5g-simulator/src/upf_fastpath
#   chmod +x oracle_setup.sh
#   sudo ./oracle_setup.sh
#
# TESTED ON: Oracle Linux 8 (ARM A1), Ubuntu 22.04 (ARM A1)
# =============================================================================

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

# Must run as root for hugepage config + VFIO binding
[[ $EUID -ne 0 ]] && error "Run as root: sudo ./oracle_setup.sh"

info "Detecting OS..."
if   [ -f /etc/oracle-release ]; then OS=oraclelinux
elif [ -f /etc/redhat-release ];  then OS=rhel
elif [ -f /etc/debian_version ]; then OS=debian
else error "Unsupported OS. Use Oracle Linux 8 or Ubuntu 22.04."
fi
info "OS: $OS"

# =============================================================================
# STEP 1: Install packages
# =============================================================================
info "Installing DPDK and build tools..."

if [[ $OS == oraclelinux || $OS == rhel ]]; then
    # Oracle Linux 8 / RHEL 8 — DPDK is in the epel repo
    dnf install -y epel-release 2>/dev/null || true
    dnf install -y dpdk dpdk-devel dpdk-tools \
        gcc g++ make cmake git pkg-config \
        numactl-devel kernel-modules-extra \
        python3-pyelftools  # needed for dpdk-devbind.py

elif [[ $OS == debian ]]; then
    # Ubuntu 22.04 — DPDK 21.11 in standard repos
    apt-get update -q
    apt-get install -y dpdk dpdk-dev libdpdk-dev \
        build-essential cmake git pkg-config \
        libnuma-dev python3-pyelftools \
        linux-modules-extra-$(uname -r) || true
fi

info "DPDK version: $(pkg-config --modversion libdpdk 2>/dev/null || echo 'check manually')"

# =============================================================================
# STEP 2: Hugepages
# =============================================================================
info "Configuring 2MB hugepages..."

# 512 hugepages × 2MB = 1GB reserved for DPDK
# On a 24GB A1, use up to 4096 (8GB) for serious packet load testing
HP_COUNT=512
HP_PATH=/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

echo $HP_COUNT > $HP_PATH
ACTUAL=$(cat $HP_PATH)
info "Hugepages requested: $HP_COUNT, allocated: $ACTUAL"
[[ $ACTUAL -lt 256 ]] && warn "Less than 256 hugepages allocated — may hit OOM in DPDK"

# Mount hugetlbfs so DPDK can mmap the pages
mkdir -p /mnt/huge
if ! mountpoint -q /mnt/huge; then
    mount -t hugetlbfs hugetlbfs /mnt/huge
    info "Mounted hugetlbfs at /mnt/huge"
fi

# Persist hugepages across reboots
if ! grep -q "hugepages" /etc/rc.local 2>/dev/null; then
    echo "echo $HP_COUNT > $HP_PATH" >> /etc/rc.local
    chmod +x /etc/rc.local
fi

# =============================================================================
# STEP 3: Load VFIO kernel module
# =============================================================================
info "Loading VFIO kernel modules..."

modprobe vfio-pci    || warn "vfio-pci load failed — may need kernel-modules-extra"
modprobe uio_pci_generic || warn "uio_pci_generic unavailable (ok if using VFIO)"

# Allow non-root VFIO access (required on some kernels)
echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null || true

# =============================================================================
# STEP 4: List available NICs
# =============================================================================
info "Network interfaces available for DPDK binding:"
if command -v dpdk-devbind.py &>/dev/null; then
    dpdk-devbind.py --status
elif command -v dpdk-devbind &>/dev/null; then
    dpdk-devbind --status
else
    warn "dpdk-devbind not found in PATH — try: /usr/share/dpdk/usertools/dpdk-devbind.py"
    ls /sys/bus/pci/devices/ | head -20
fi

cat <<'VFIO_HINT'

  ┌─────────────────────────────────────────────────────────────────────┐
  │  HOW TO BIND A NIC TO VFIO (skip if using --vdev net_tap)          │
  │                                                                     │
  │  1. Find your NIC's PCI address from the list above:               │
  │     e.g., 0000:00:03.0 (virtio-net-pci on Oracle Cloud)           │
  │                                                                     │
  │  2. Bring down the NIC in Linux:                                   │
  │     ip link set eth0 down                                          │
  │                                                                     │
  │  3. Bind to VFIO:                                                  │
  │     dpdk-devbind.py --bind=vfio-pci 0000:00:03.0                  │
  │                                                                     │
  │  4. Verify:                                                         │
  │     dpdk-devbind.py --status                                       │
  │                                                                     │
  │  NOTE: After binding, the NIC disappears from 'ip link show'.     │
  │  DPDK owns it exclusively — no kernel driver, no SSH over it.     │
  │  Use a SECOND NIC for SSH, or use --vdev net_tap for testing.      │
  └─────────────────────────────────────────────────────────────────────┘

VFIO_HINT

# =============================================================================
# STEP 5: Build the UPF fast-path with real DPDK
# =============================================================================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
info "Building UPF fast-path in: $SCRIPT_DIR"
cd "$SCRIPT_DIR"

if pkg-config --exists libdpdk 2>/dev/null; then
    info "Building with REAL DPDK (ENABLE_DPDK=1)..."
    make clean 2>/dev/null || true
    make ENABLE_DPDK=1
    info "Build complete: upf_demo, upf_benchmark"
else
    warn "pkg-config cannot find libdpdk — building in simulation mode"
    warn "Try: export PKG_CONFIG_PATH=/usr/lib64/pkgconfig:$PKG_CONFIG_PATH"
    make
fi

# =============================================================================
# STEP 6: Run instructions
# =============================================================================
cat <<'RUN_GUIDE'

╔══════════════════════════════════════════════════════════════════════════════╗
║  HOW TO RUN ON ORACLE CLOUD                                                ║
╠══════════════════════════════════════════════════════════════════════════════╣
║                                                                              ║
║  OPTION A — Virtual TAP device (no NIC binding, easiest for learning):      ║
║  ─────────────────────────────────────────────────────────────────────       ║
║  sudo ./upf_benchmark -l 0-1 -n 2 --vdev net_tap0,iface=tap0 -- --ues 100 ║
║                                                                              ║
║  EAL args (before --):                                                       ║
║    -l 0-1       : use lcores 0 and 1 (lcore 0 = main, lcore 1 = poll loop) ║
║    -n 2         : 2 memory channels (DDR channels on the board)              ║
║    --vdev net_tap0,iface=tap0  : virtual tap device for testing              ║
║                                                                              ║
║  App args (after --):                                                        ║
║    --ues 100    : simulate 100 UE sessions                                   ║
║                                                                              ║
║  OPTION B — Real NIC with VFIO (after binding with dpdk-devbind.py):        ║
║  ─────────────────────────────────────────────────────────────────────       ║
║  sudo ./upf_benchmark -l 0-3 -n 4 -a 0000:00:03.0 -- --ues 1000            ║
║                                                                              ║
║    -l 0-3       : 4 lcores (1 main + 3 poll workers)                        ║
║    -a 0000:00:03.0  : PCI address of VFIO-bound NIC                         ║
║                                                                              ║
║  OPTION C — Demo mode (socket vs DPDK comparison):                           ║
║  ─────────────────────────────────────────────────────────────────────       ║
║  sudo ./upf_demo -l 0-1 -n 2 --vdev net_tap0 -- --dpdk                     ║
║                                                                              ║
║  EXPECTED RESULTS on Oracle Cloud A1 (ARM, 4 cores):                        ║
║  ─────────────────────────────────────────────────────────────────────       ║
║  Simulation (Mac):   Socket ~2-5 Mpps,  DPDK sim ~8-15 Mpps                ║
║  Real DPDK + tap:    Socket ~1-2 Mpps,  DPDK    ~5-10 Mpps                 ║
║  Real DPDK + NIC:    Socket ~200 Kpps,  DPDK    ~5-40 Mpps                 ║
║                                                                              ║
║  The NIC column shows the real hardware gain: kernel path tops out at        ║
║  ~1Mpps (syscall + interrupt overhead). DPDK poll-mode: 40+ Mpps.           ║
║                                                                              ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  KERNEL ISOLATION (for production-grade numbers):                            ║
║  ─────────────────────────────────────────────────────────────────────       ║
║  Add to /etc/default/grub:                                                   ║
║    GRUB_CMDLINE_LINUX="isolcpus=1,2,3 nohz_full=1,2,3 rcu_nocbs=1,2,3"    ║
║  Then: sudo update-grub && sudo reboot                                       ║
║  After isolation, lcore scheduling jitter: <50ns (vs ~1μs without)          ║
╚══════════════════════════════════════════════════════════════════════════════╝

RUN_GUIDE

info "Setup complete. See above for run instructions."
