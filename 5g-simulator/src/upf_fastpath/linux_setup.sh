#!/usr/bin/env bash
# =============================================================================
# linux_setup.sh — DPDK setup for RHEL 8/9, Oracle Linux 8/9, Ubuntu 22/24
#
# SUPPORTED:
#   RHEL 8 / RHEL 9            (bare metal, AWS EC2, on-prem)
#   Oracle Linux 8 / 9
#   Ubuntu 22.04 / 24.04
#
# WHAT THIS DOES:
#   1. Detects your OS and installs DPDK dev libraries
#   2. Falls back to building DPDK 23.11 LTS from source if packages are missing
#   3. Configures 2MB hugepages (DPDK memory requirement)
#   4. Mounts hugepages filesystem
#   5. Builds UPF fast-path with real DPDK (ENABLE_DPDK=1)
#   6. Prints run instructions
#
# USAGE:
#   git clone https://github.com/abhichhn93/4g-5g-ims-simulators
#   cd 4g-5g-ims-simulators/5g-simulator/src/upf_fastpath
#   chmod +x linux_setup.sh
#   sudo ./linux_setup.sh
#
# HARDWARE REQUIREMENTS:
#   RAM:     >= 4GB (DPDK hugepages need at least 1GB)
#   CPU:     >= 2 cores (one for main, one for DPDK lcore)
#   NIC:     Any Intel/Mellanox NIC with VFIO support
#            OR use --vdev net_tap for testing without NIC binding
#
# TESTED ON:
#   RHEL 8.9 (EC2 c5.xlarge, x86_64)
#   RHEL 9.3 (EC2 c6i.xlarge, x86_64)
#   Oracle Linux 8 (OCI A1 ARM, aarch64)
#   Ubuntu 22.04 LTS (x86_64 and ARM)
# =============================================================================

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }
step()  { echo -e "\n${BLUE}══════════════════════════════════════════${NC}"; echo -e "${BLUE}  $*${NC}"; echo -e "${BLUE}══════════════════════════════════════════${NC}"; }

[[ $EUID -ne 0 ]] && error "Run as root: sudo ./linux_setup.sh"

ARCH=$(uname -m)   # x86_64 or aarch64
info "Architecture: $ARCH"

# ─── OS Detection ─────────────────────────────────────────────────────────────
step "Step 1: Detecting OS"

OS_ID=""
OS_VER=""
if [ -f /etc/os-release ]; then
    . /etc/os-release
    OS_ID="${ID:-}"
    OS_VER="${VERSION_ID:-}"
fi

if   [[ -f /etc/oracle-release ]];      then OS_FAMILY="oracle";  OS_NAME="Oracle Linux $OS_VER"
elif [[ "$OS_ID" == "rhel" ]];          then OS_FAMILY="rhel";    OS_NAME="RHEL $OS_VER"
elif [[ "$OS_ID" == "centos" ]];        then OS_FAMILY="rhel";    OS_NAME="CentOS $OS_VER"
elif [[ "$OS_ID" == "rocky" ]];         then OS_FAMILY="rhel";    OS_NAME="Rocky Linux $OS_VER"
elif [[ "$OS_ID" == "almalinux" ]];     then OS_FAMILY="rhel";    OS_NAME="AlmaLinux $OS_VER"
elif [[ "$OS_ID" == "ubuntu" ]];        then OS_FAMILY="debian";  OS_NAME="Ubuntu $OS_VER"
elif [[ "$OS_ID" == "debian" ]];        then OS_FAMILY="debian";  OS_NAME="Debian $OS_VER"
else error "Unsupported OS: $OS_ID $OS_VER. Use RHEL 8/9, Oracle Linux 8/9, or Ubuntu 22.04+."
fi

RHEL_MAJOR=${OS_VER%%.*}   # "8" from "8.9", "9" from "9.3"
info "OS: $OS_NAME"

# ─── Package installation ─────────────────────────────────────────────────────
step "Step 2: Installing DPDK and build tools"

install_packages_rhel() {
    info "Enabling EPEL repository..."
    dnf install -y epel-release 2>/dev/null || \
        dnf install -y https://dl.fedoraproject.org/pub/epel/epel-release-latest-${RHEL_MAJOR}.noarch.rpm || \
        warn "EPEL install failed — will try without it"

    # Enable CodeReady Linux Builder (has dpdk-devel)
    if [[ "$RHEL_MAJOR" == "8" ]]; then
        dnf config-manager --set-enabled powertools 2>/dev/null || \
        dnf config-manager --set-enabled codeready-builder-for-rhel-8-rhui-rpms 2>/dev/null || \
        subscription-manager repos --enable codeready-builder-for-rhel-8-rhui-rpms 2>/dev/null || \
        warn "Could not enable CRB/PowerTools — dpdk-devel might be missing"
    elif [[ "$RHEL_MAJOR" == "9" ]]; then
        dnf config-manager --set-enabled crb 2>/dev/null || \
        subscription-manager repos --enable codeready-builder-for-rhel-9-rhui-rpms 2>/dev/null || \
        warn "Could not enable CRB — dpdk-devel might be missing"
    fi

    info "Installing build tools and DPDK..."
    dnf install -y \
        gcc gcc-c++ make cmake git pkg-config \
        numactl-devel python3-pyelftools \
        kernel-headers kernel-devel \
        pciutils rdma-core-devel || true

    # Try installing DPDK packages (might not exist on all RHEL versions)
    if dnf install -y dpdk dpdk-devel dpdk-tools 2>/dev/null; then
        info "DPDK installed from packages"
        PKG_CONFIG_PATH="/usr/lib64/pkgconfig:/usr/local/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
        export PKG_CONFIG_PATH
    else
        warn "DPDK packages not available in repos — will build from source (see Step 2b)"
        BUILD_FROM_SOURCE=1
    fi
}

install_packages_debian() {
    apt-get update -q
    apt-get install -y \
        build-essential cmake git pkg-config \
        dpdk dpdk-dev libdpdk-dev \
        libnuma-dev python3-pyelftools \
        linux-modules-extra-$(uname -r) || true

    if pkg-config --exists libdpdk 2>/dev/null; then
        info "DPDK installed from packages"
    else
        warn "DPDK dev headers missing — will build from source"
        BUILD_FROM_SOURCE=1
    fi
}

BUILD_FROM_SOURCE=0
if [[ "$OS_FAMILY" == "rhel" || "$OS_FAMILY" == "oracle" ]]; then
    install_packages_rhel
else
    install_packages_debian
fi

# ─── Build DPDK from source (fallback) ───────────────────────────────────────
build_dpdk_from_source() {
    step "Step 2b: Building DPDK 23.11 LTS from source"
    warn "This takes 5-10 minutes. This is the fallback path when packages are unavailable."

    # DPDK 23.11 is the current LTS (Long Term Support) release
    DPDK_VER="23.11"
    DPDK_DIR="/opt/dpdk-${DPDK_VER}"

    # Install meson + ninja (DPDK's build system since DPDK 20.x)
    if ! command -v meson &>/dev/null; then
        if [[ "$OS_FAMILY" == "rhel" || "$OS_FAMILY" == "oracle" ]]; then
            dnf install -y meson ninja-build 2>/dev/null || \
            python3 -m pip install meson ninja
        else
            apt-get install -y meson ninja-build
        fi
    fi

    if [[ ! -d "$DPDK_DIR" ]]; then
        info "Downloading DPDK $DPDK_VER..."
        curl -sL "https://fast.dpdk.org/rel/dpdk-${DPDK_VER}.tar.xz" -o /tmp/dpdk.tar.xz
        tar -xf /tmp/dpdk.tar.xz -C /opt
        mv "/opt/dpdk-${DPDK_VER}"-* "$DPDK_DIR" 2>/dev/null || true
    fi

    info "Building DPDK (this takes a few minutes)..."
    cd "$DPDK_DIR"

    # Build with minimal drivers for learning — saves build time
    # In production, enable specific PMDs for your NIC (e.g., -Denable_drivers=net/ixgbe)
    meson setup --prefix=/usr/local \
        -Denable_drivers=net/virtio,net/tap,net/e1000,net/ixgbe,net/mlx5 \
        -Dtests=false \
        build
    ninja -C build -j$(nproc)
    ninja -C build install
    ldconfig

    PKG_CONFIG_PATH="/usr/local/lib64/pkgconfig:/usr/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    export PKG_CONFIG_PATH

    cd -
    info "DPDK $DPDK_VER built and installed to /usr/local"
}

if [[ $BUILD_FROM_SOURCE -eq 1 ]]; then
    build_dpdk_from_source
fi

# Verify DPDK is findable
if pkg-config --exists libdpdk 2>/dev/null; then
    DPDK_VERSION=$(pkg-config --modversion libdpdk 2>/dev/null || echo "unknown")
    info "DPDK found: version $DPDK_VERSION"
    info "CFLAGS: $(pkg-config --cflags libdpdk | head -c 80)..."
else
    error "DPDK not found via pkg-config. Try: export PKG_CONFIG_PATH=/usr/local/lib64/pkgconfig && ./linux_setup.sh"
fi

# ─── Hugepages ────────────────────────────────────────────────────────────────
step "Step 3: Configuring 2MB hugepages"

# How many hugepages to allocate depends on available RAM:
#   4GB RAM  → 512 hugepages (1GB for DPDK)
#   8GB RAM  → 1024 hugepages (2GB for DPDK)
#   24GB RAM → 4096 hugepages (8GB for DPDK) [Oracle A1 max]
TOTAL_RAM_MB=$(grep MemTotal /proc/meminfo | awk '{print $2/1024}' | cut -d. -f1)
if   [[ $TOTAL_RAM_MB -ge 16000 ]]; then HP_COUNT=2048
elif [[ $TOTAL_RAM_MB -ge 8000  ]]; then HP_COUNT=1024
elif [[ $TOTAL_RAM_MB -ge 4000  ]]; then HP_COUNT=512
else                                      HP_COUNT=256
fi

info "RAM: ${TOTAL_RAM_MB}MB → allocating ${HP_COUNT} hugepages ($(( HP_COUNT * 2 ))MB for DPDK)"

HP_PATH=/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
echo $HP_COUNT > $HP_PATH
ACTUAL=$(cat $HP_PATH)
info "Hugepages allocated: $ACTUAL / $HP_COUNT requested"
[[ $ACTUAL -lt 128 ]] && error "Only $ACTUAL hugepages allocated — need at least 128. Check dmesg for memory errors."

# Mount hugetlbfs so DPDK can find the hugepages
HP_MOUNT=/mnt/huge
mkdir -p $HP_MOUNT
if ! mountpoint -q $HP_MOUNT; then
    mount -t hugetlbfs nodev $HP_MOUNT
    info "Hugetlbfs mounted at $HP_MOUNT"
else
    info "Hugetlbfs already mounted at $HP_MOUNT"
fi

# Make hugepages persistent across reboots
if ! grep -q "hugetlbfs" /etc/fstab; then
    echo "nodev   /mnt/huge   hugetlbfs   defaults   0   0" >> /etc/fstab
    echo "vm.nr_hugepages = $HP_COUNT" >> /etc/sysctl.d/99-dpdk.conf
    info "Hugepages configured in /etc/fstab and /etc/sysctl.d/99-dpdk.conf (persistent)"
fi

# ─── VFIO kernel module ───────────────────────────────────────────────────────
step "Step 4: Loading VFIO kernel module"

# vfio-pci: lets DPDK take ownership of a NIC directly (kernel-bypass)
# vfio: the IOMMU-based driver framework
modprobe vfio-pci  2>/dev/null && info "vfio-pci loaded" || warn "vfio-pci not available (OK if using net_tap)"
modprobe vfio      2>/dev/null && info "vfio loaded" || true

# Enable VFIO without IOMMU (for VMs that don't have IOMMU passthrough)
# This is common on EC2, Oracle Cloud, and most VMs
if [[ -f /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]]; then
    echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
    info "VFIO no-IOMMU mode enabled (needed for most VMs)"
fi

# ─── Build UPF fast-path ──────────────────────────────────────────────────────
step "Step 5: Building UPF fast-path with ENABLE_DPDK=1"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Set PKG_CONFIG_PATH for all possible DPDK install locations
export PKG_CONFIG_PATH="/usr/lib64/pkgconfig:/usr/lib/pkgconfig:/usr/local/lib64/pkgconfig:/usr/local/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

if ! pkg-config --exists libdpdk; then
    error "libdpdk not found after setup. Run: pkg-config --list-all | grep dpdk"
fi

make clean 2>/dev/null || true

info "Building with real DPDK..."
make ENABLE_DPDK=1

info "Build complete!"
ls -lh upf_demo upf_benchmark upf_real 2>/dev/null

# ─── Show NIC binding instructions ───────────────────────────────────────────
step "Step 6: NIC status and binding"

# dpdk-devbind.py shows which NICs are available and their current driver
DEVBIND=""
for p in /usr/sbin /usr/bin /usr/share/dpdk/usertools /opt/dpdk-23.11/usertools; do
    [[ -f "$p/dpdk-devbind.py" ]] && DEVBIND="$p/dpdk-devbind.py" && break
done

if [[ -n "$DEVBIND" ]]; then
    info "NIC status (before binding to VFIO):"
    python3 "$DEVBIND" --status 2>/dev/null || true
else
    warn "dpdk-devbind.py not found — skipping NIC status"
fi

# ─── Run instructions ─────────────────────────────────────────────────────────
cat <<'RUN_GUIDE'

╔══════════════════════════════════════════════════════════════════════════════╗
║          UPF FAST-PATH — HOW TO RUN ON YOUR LINUX SERVER                   ║
╠══════════════════════════════════════════════════════════════════════════════╣
║                                                                              ║
║  OPTION A — Virtual TAP (no NIC binding, works immediately):                 ║
║  ─────────────────────────────────────────────────────────────────────       ║
║  sudo ./upf_real   -l 0-1 -n 2 --vdev net_tap0,iface=tap0 -- --ues 100    ║
║  sudo ./upf_benchmark -l 0-1 -n 2 --vdev net_tap0,iface=tap0 -- --ues 100 ║
║                                                                              ║
║  Use this first — it confirms DPDK is working before touching your NIC.     ║
║                                                                              ║
║  EAL arguments (before the --):                                              ║
║    -l 0-1       use CPU cores 0 and 1                                       ║
║    -n 2         2 memory channels (match your DDR config)                   ║
║    --vdev net_tap0,iface=tap0   virtual tap device (no VFIO needed)        ║
║                                                                              ║
║  App arguments (after the --):                                               ║
║    --ues 100    simulate 100 UE sessions                                    ║
║                                                                              ║
║  OPTION B — Real NIC with VFIO (for actual performance numbers):             ║
║  ─────────────────────────────────────────────────────────────────────       ║
║                                                                              ║
║  Step 1: Find your NIC's PCI address                                         ║
║    lspci | grep -i ethernet                                                  ║
║    → 00:03.0 Ethernet controller: Intel Corporation ...                     ║
║                                                                              ║
║  Step 2: Check the NIC is not the one you SSH over!                          ║
║    ip link show  → note which interface has your SSH IP                      ║
║    # NEVER bind the SSH interface to VFIO — you'll lose the connection       ║
║                                                                              ║
║  Step 3: Bind the SECOND NIC to VFIO                                         ║
║    dpdk-devbind.py --bind vfio-pci 0000:00:03.0                             ║
║    dpdk-devbind.py --status   # confirm it shows "vfio-pci"                 ║
║                                                                              ║
║  Step 4: Run with real NIC                                                   ║
║    sudo ./upf_benchmark -l 0-3 -n 4 -a 0000:00:03.0 -- --ues 1000          ║
║                                                                              ║
║  Step 5: Release NIC when done                                               ║
║    dpdk-devbind.py --bind virtio-net 0000:00:03.0  # or ixgbe, mlx5, etc.  ║
║                                                                              ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  EXPECTED RESULTS on a 4-core RHEL server:                                  ║
║  ─────────────────────────────────────────────────────────────────────       ║
║                                                                              ║
║  net_tap (virtual):   ~5-15 Mpps (DPDK poll-mode, no real NIC overhead)    ║
║  Real NIC (VFIO):     ~10-40 Mpps (depends on NIC and CPU frequency)       ║
║  Kernel socket path:  ~0.2-1 Mpps (comparison baseline)                    ║
║                                                                              ║
║  To see the comparison: run upf_benchmark with and without --dpdk flag      ║
║                                                                              ║
╠══════════════════════════════════════════════════════════════════════════════╣
║  PERFORMANCE TUNING (optional, for production-grade numbers):               ║
║  ─────────────────────────────────────────────────────────────────────       ║
║                                                                              ║
║  1. CPU isolation (prevents OS scheduler jitter on DPDK lcores):            ║
║     Add to /etc/default/grub GRUB_CMDLINE_LINUX:                           ║
║     isolcpus=1,2,3 nohz_full=1,2,3 rcu_nocbs=1,2,3                        ║
║     Then: grub2-mkconfig -o /boot/grub2/grub.cfg && reboot                 ║
║                                                                              ║
║  2. NUMA awareness (-n 4 uses 4 memory channels):                           ║
║     sudo ./upf_benchmark -l 0-7 -n 4 --socket-mem 2048 ...                 ║
║                                                                              ║
║  3. Disable C-states (prevent CPU from sleeping):                           ║
║     for i in /sys/devices/system/cpu/cpu*/cpuidle/state*/disable;           ║
║       do echo 1 > $i; done                                                  ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝

RUN_GUIDE

info "Setup complete. See instructions above."
info "PKG_CONFIG_PATH=$PKG_CONFIG_PATH"
