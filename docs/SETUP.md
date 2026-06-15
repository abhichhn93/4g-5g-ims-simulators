# Setup Guide — Mac, Linux, Windows

## macOS

```bash
# 1. Install tools (one time)
xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install cmake

# 2. Clone and build
git clone https://github.com/YOUR_USERNAME/4g-simulator.git
cd 4g-simulator
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(sysctl -n hw.ncpu)

# 3. Run
./mme_sim      # 4G EPC
```

IMS/VoLTE (`mme_ims`) is a separate sibling project — see `../ims-simulator/`.

---

## Linux (Ubuntu/Debian)

```bash
# 1. Install tools
sudo apt update
sudo apt install -y git build-essential cmake g++

# 2. Clone and build
git clone https://github.com/YOUR_USERNAME/4g-simulator.git
cd 4g-simulator
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# 3. Run
./mme_sim
```

IMS/VoLTE (`mme_ims`) is a separate sibling project — see `../ims-simulator/`.

---

## Windows (WSL2)

```powershell
# 1. Enable WSL2 (run in PowerShell as Admin)
wsl --install
# Restart PC when prompted, then open Ubuntu from Start menu
```

```bash
# 2. Inside Ubuntu WSL2 terminal
sudo apt update && sudo apt install -y git build-essential cmake g++
git clone https://github.com/YOUR_USERNAME/4g-simulator.git
cd 4g-simulator && mkdir build && cd build
cmake .. && make -j$(nproc)
./mme_sim
```

---

## VS Code (any platform)

1. Install [VS Code](https://code.visualstudio.com/)
2. Install extension: **C/C++** (Microsoft)
3. Install extension: **CMake Tools** (Microsoft)
4. Open folder: `File → Open Folder → 4g-simulator/`
5. CMake Tools detects `CMakeLists.txt` automatically — click **Build**
6. Open integrated terminal: `` Ctrl+` ``
7. Run: `./build/mme_sim`

---

## Wireshark Setup

1. Download [Wireshark](https://www.wireshark.org/download.html)
2. Load Lua dissector:
   - `Edit → Preferences → Advanced → Search: lua`
   - Or: copy `mme_sim_dissector.lua` to Wireshark plugins folder:
     - Mac: `~/.local/lib/wireshark/plugins/`
     - Linux: `~/.local/lib/wireshark/plugins/`
     - Windows: `%APPDATA%\Wireshark\plugins\`
3. Restart Wireshark

**Capture command (Mac/Linux):**
```bash
sudo tcpdump -i lo0 \
  'port 36412 or port 3868 or port 2123 or port 2124 or port 3869' \
  -w mme_capture.pcap
```

**Windows (WSL2):** Use Wireshark on Windows, capture `loopback` adapter.
