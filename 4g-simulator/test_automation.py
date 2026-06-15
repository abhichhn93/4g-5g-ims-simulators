import subprocess
import time
import os

def test_4g_attach_flow():
    """
    INTERVIEW TIP: This script demonstrates 'Black Box Testing' of a Telecom Node.
    It triggers a command, captures the output, and could potentially verify the PCAP.
    """
    print("\n[PYTHON-AUTO] Starting 4G Attach Automation Test...")
    
    # Start the simulator process
    # In a real scenario, we would use pexpect to interact with the CLI
    cmd = "echo 'CR 1\nQUIT' | ./build/mme_sim"
    
    try:
        result = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=10)
        
        # Logic to verify the 'Interview' level requirements
        if "ATTACH COMPLETE" in result.stdout:
            print("[SUCCESS] UE Registered successfully in the logs.")
        else:
            print("[FAILURE] Attach flow did not complete.")
            
        if "10.0.0.1" in result.stdout:
            print("[SUCCESS] IP Allocation Verified: 10.0.0.1")
            
    except subprocess.TimeoutExpired:
        print("[ERROR] Simulator hung during attach flow.")

if __name__ == "__main__":
    # This proves you understand the 'DevOps' side of Telecom
    # You would put this on your LinkedIn to show 'C++ Sim + Python Automation'
    if not os.path.exists("./build/mme_sim"):
        print("Please build the project first using cmake and make.")
    else:
        test_4g_attach_flow()