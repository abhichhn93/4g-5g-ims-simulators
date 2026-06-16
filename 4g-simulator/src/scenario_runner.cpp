// ============================================================
// Scenario Runner — YAML-driven end-to-end flow automation
//
// Parses a minimal YAML scenario file and drives mme_sim via its
// CLI command queue. No external libraries (no yaml-cpp, no boost).
// Hand-rolled parser handles the flat subset of YAML we need.
//
// SCENARIO FILE FORMAT:
//   name: "VoLTE-scale-test"
//   mode: ENGINEER
//   steps:
//     - cmd: CR 1
//       expect: REGISTERED
//     - cmd: BULK 10
//       delay_ms: 500
//     - cmd: TAU 1
//     - cmd: HO 1
//     - cmd: STATUS
//     - cmd: QUIT
//
// INTERVIEW VALUE:
//   "Can you drive your simulator programmatically?"
//   "Yes — scenario_runner reads a YAML file and pipes commands to
//    mme_sim stdin. This is the same pattern used by test harnesses
//    in production: Pytest drives the CLI, checks stdout for keywords,
//    fails the test if 'REGISTERED' doesn't appear within N seconds.
//    CI runs this on every push — the .github/workflows/ CI file
//    already uses this pattern for the smoke test."
//
// PYTHON AUTOMATION:
//   For richer automation (parallel scenarios, metric assertions):
//     from subprocess import Popen, PIPE
//     proc = Popen(['./mme_sim'], stdin=PIPE, stdout=PIPE, text=True)
//     proc.stdin.write('CR 1\n'); proc.stdin.flush()
//     for line in proc.stdout:
//         if 'REGISTERED' in line: break
//   See test_automation.py in the 4g-simulator root for a working example.
// ============================================================
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

struct Step {
    std::string cmd;
    std::string expect;    // optional keyword to wait for in output
    int         delay_ms{0};
};

struct Scenario {
    std::string        name;
    std::string        mode{"ENGINEER"};
    std::vector<Step>  steps;
};

// ── Minimal flat-YAML parser ──────────────────────────────────
// Handles:
//   key: value
//   - key: value   (list items)
// Ignores comments (#) and blank lines.
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}

static std::string stripQuotes(const std::string& s) {
    if (s.size() >= 2 && ((s.front()=='"'&&s.back()=='"') ||
                           (s.front()=='\''&&s.back()=='\'')))
        return s.substr(1, s.size()-2);
    return s;
}

static bool parseYaml(const std::string& filename, Scenario& sc) {
    std::ifstream f(filename);
    if (!f) { std::cerr << "ERROR: cannot open " << filename << "\n"; return false; }

    Step cur_step;
    bool in_steps = false;
    bool step_open = false;
    std::string line;

    while (std::getline(f, line)) {
        // strip comment
        size_t cmt = line.find('#');
        if (cmt != std::string::npos) line = line.substr(0, cmt);
        std::string t = trim(line);
        if (t.empty()) continue;

        // detect list item start
        if (t[0] == '-') {
            if (!in_steps) continue;
            if (step_open) sc.steps.push_back(cur_step);
            cur_step = Step{};
            step_open = true;
            t = trim(t.substr(1));
        }

        // parse key: value
        size_t colon = t.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(t.substr(0, colon));
        std::string val = stripQuotes(trim(t.substr(colon + 1)));

        if (!in_steps) {
            if (key == "name")       sc.name = val;
            else if (key == "mode") sc.mode = val;
            else if (key == "steps") in_steps = true;
        } else {
            if (key == "cmd")       cur_step.cmd = val;
            else if (key == "expect")   cur_step.expect = val;
            else if (key == "delay_ms") {
                try { cur_step.delay_ms = std::stoi(val); } catch (...) {}
            }
        }
    }
    if (step_open) sc.steps.push_back(cur_step);
    return true;
}

// ── Runner: write commands to stdout (pipe to mme_sim) ────────
static void run(const Scenario& sc) {
    std::cerr << "=== Scenario: " << sc.name << " ===\n";
    std::cerr << "    mode=" << sc.mode << "  steps=" << sc.steps.size() << "\n";

    // First command: set mode
    std::cout << "MODE " << sc.mode << "\n" << std::flush;

    for (size_t i = 0; i < sc.steps.size(); ++i) {
        const auto& s = sc.steps[i];
        std::cerr << "[" << (i+1) << "/" << sc.steps.size() << "] cmd=" << s.cmd << "\n";

        if (s.delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(s.delay_ms));

        std::cout << s.cmd << "\n" << std::flush;

        // If 'expect' is set, we'd normally read stdout from mme_sim and
        // check for the keyword. When piped: mme_sim output goes elsewhere.
        // The 'expect' field is preserved for Python automation wrappers
        // (see test_automation.py) that DO capture stdout.
        if (!s.expect.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    std::cerr << "=== Scenario complete ===\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: scenario_runner <scenario.yaml>\n";
        std::cerr << "       Outputs CLI commands to stdout — pipe to mme_sim:\n";
        std::cerr << "       scenario_runner scenario.yaml | ./mme_sim\n";
        std::cerr << "\nPython automation alternative (richer, can assert on output):\n";
        std::cerr << "       python3 test_automation.py scenario.yaml\n";
        return 1;
    }

    Scenario sc;
    if (!parseYaml(argv[1], sc)) return 1;
    run(sc);
    return 0;
}
