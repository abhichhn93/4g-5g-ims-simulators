#!/usr/bin/env python3
# run_scenario.py — YAML-driven scenario runner for all three simulators.
# Reads a scenario YAML file and sends commands to running simulators via stdin
# or socket. Supports 4G, 5G, and IMS scenarios in one file.
# Usage: python3 tools/run_scenario.py scenarios/e2e_volte.yaml
#        python3 tools/run_scenario.py --sim 4g scenarios/attach_tau_ho.yaml

import argparse
import pathlib
import subprocess
import sys
import time
import signal

try:
    import yaml
except ImportError:
    print("ERROR: pyyaml not installed — run: pip install pyyaml")
    sys.exit(1)

REPO = pathlib.Path(__file__).parent.parent.resolve()

# ── Default simulator paths ────────────────────────────────────────────────────
SIM_BINS = {
    "4g":  REPO / "4g-simulator"  / "build" / "mme_sim",
    "5g":  REPO / "5g-simulator"  / "build" / "gnb_sim",
    "ims": REPO / "ims-simulator" / "build" / "mme_ims",
}

def load_scenario(path: pathlib.Path) -> dict:
    with open(path) as f:
        return yaml.safe_load(f)

def run_steps(steps: list, proc: subprocess.Popen, verbose: bool):
    for step in steps:
        cmd    = str(step.get("cmd", "")).strip()
        expect = step.get("expect", "")
        delay  = float(step.get("delay_ms", 200)) / 1000.0

        if not cmd:
            continue

        print(f"  → {cmd}")
        proc.stdin.write((cmd + "\n").encode())
        proc.stdin.flush()
        time.sleep(delay)

        if expect and verbose:
            print(f"    (expected: {expect})")

def run_scenario(scenario: dict, sim: str, verbose: bool):
    name   = scenario.get("scenario", scenario.get("name", "unnamed"))
    mode   = scenario.get("mode", "ENGINEER")
    steps  = scenario.get("steps", [])

    # Allow override of which simulator to run
    target_sim = scenario.get("sim", sim)
    bin_path   = SIM_BINS.get(target_sim)

    if not bin_path or not bin_path.exists():
        print(f"ERROR: simulator binary not found for '{target_sim}': {bin_path}")
        print(f"       Build it first: cd {target_sim}-simulator && cmake -B build && cmake --build build")
        sys.exit(1)

    print(f"\n{'='*60}")
    print(f"Scenario: {name}  (sim={target_sim}, mode={mode})")
    print(f"{'='*60}")

    cwd = bin_path.parent
    proc = subprocess.Popen(
        [str(bin_path)],
        stdin  = subprocess.PIPE,
        stdout = None if verbose else subprocess.DEVNULL,
        stderr = None if verbose else subprocess.DEVNULL,
        cwd    = str(cwd),
    )

    def _cleanup(sig, frame):
        proc.terminate()
        sys.exit(0)
    signal.signal(signal.SIGINT, _cleanup)

    try:
        # Set mode first
        proc.stdin.write((f"MODE {mode}\n").encode())
        proc.stdin.flush()
        time.sleep(0.2)

        run_steps(steps, proc, verbose)

        # Send QUIT and wait
        proc.stdin.write(b"QUIT\n")
        proc.stdin.flush()
        proc.wait(timeout=10)
        print(f"\n✓ Scenario '{name}' completed (exit={proc.returncode})")

    except subprocess.TimeoutExpired:
        print(f"WARNING: simulator did not exit cleanly — killing")
        proc.kill()
    except BrokenPipeError:
        print(f"ERROR: simulator exited prematurely")

def main():
    parser = argparse.ArgumentParser(
        description="YAML-driven scenario runner for 4G/5G/IMS simulators",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Scenario YAML format:
  scenario: full_e2e_volte
  sim: ims          # 4g | 5g | ims  (override per-scenario)
  mode: BEGINNER    # BEGINNER | ENGINEER | INTERVIEW
  steps:
    - cmd: REG ALL
      delay_ms: 500
    - cmd: CALL A B
      expect: "INVITE sent"
      delay_ms: 1000
    - cmd: BYE

Built-in examples in 4g-simulator/scenarios/:
  basic_attach.yaml
  mobility_and_handover.yaml
  bulk_load_test.yaml
""")
    parser.add_argument("scenario", help="Path to scenario YAML file")
    parser.add_argument("--sim",  default="4g", choices=["4g","5g","ims"],
                        help="Which simulator to run (default: 4g, overridden by scenario yaml)")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Show simulator output inline")
    args = parser.parse_args()

    path = pathlib.Path(args.scenario)
    if not path.exists():
        # Try relative to repo root
        path = REPO / args.scenario
    if not path.exists():
        print(f"ERROR: scenario file not found: {args.scenario}")
        sys.exit(1)

    scenario = load_scenario(path)
    run_scenario(scenario, args.sim, args.verbose)

if __name__ == "__main__":
    main()
