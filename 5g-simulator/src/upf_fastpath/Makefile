# =============================================================================
# UPF Fast-Path Makefile
#
# Targets:
#   upf_demo       — interactive demo comparing socket vs DPDK path
#   upf_benchmark  — automated 2M-packet throughput/latency benchmark
#
# Build (Mac, simulation mode):
#   make
#   ./upf_demo --dpdk
#   ./upf_benchmark --ues 200
#
# Build with real DPDK (Oracle Cloud only):
#   make ENABLE_DPDK=1
#   DPDK must be installed: see oracle_setup.sh
# =============================================================================

CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -pthread
INCLUDES := -I.

# Real DPDK path (Oracle Cloud only — requires dpdk-dev package)
ifeq ($(ENABLE_DPDK),1)
  PKG_CFLAGS  := $(shell pkg-config --cflags libdpdk 2>/dev/null)
  PKG_LIBS    := $(shell pkg-config --libs   libdpdk 2>/dev/null)
  CXXFLAGS    += $(PKG_CFLAGS) -DENABLE_DPDK=1
  LDFLAGS     += $(PKG_LIBS)
  $(info >>> Building with REAL DPDK (Oracle Cloud mode) <<<)
else
  $(info >>> Building with SIMULATION (Mac mode, no DPDK needed) <<<)
endif

HEADERS := packet.h pfcp_rules.h ring_buffer.h mbuf_pool.h \
           stats.h dpdk_path.h socket_path.h dpdk_compat.h

.PHONY: all clean run-demo run-bench run-real

all: upf_demo upf_benchmark upf_real

upf_demo: upf_fastpath_demo.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(LDFLAGS)
	@echo "Built: upf_demo"

upf_benchmark: benchmark.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(LDFLAGS)
	@echo "Built: upf_benchmark"

# upf_real: always built, but only does real DPDK when ENABLE_DPDK=1.
# On Mac (simulation mode): binary runs and prints instructions, no crash.
# On Oracle Cloud: make ENABLE_DPDK=1 → calls rte_eal_init, real lcore, real NIC.
upf_real: upf_dpdk_real.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(LDFLAGS)
	@echo "Built: upf_real"

run-demo: upf_demo
	./upf_demo --dpdk

run-bench: upf_benchmark
	./upf_benchmark --ues 100

# Run with virtual tap device (no physical NIC needed — works on Oracle Cloud):
# sudo ./upf_real -l 0-1 -n 2 --vdev net_tap0,iface=tap0 -- --ues 100
run-real: upf_real
	@echo "Run with: sudo ./upf_real -l 0-1 -n 2 --vdev net_tap0,iface=tap0 -- --ues 100"

clean:
	rm -f upf_demo upf_benchmark upf_real
