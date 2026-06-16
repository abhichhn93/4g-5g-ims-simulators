---
name: Bug report
about: Something crashes, wrong pcap output, or protocol mismatch
labels: bug
---

**Simulator affected**
- [ ] 4g-simulator
- [ ] ims-simulator
- [ ] 5g-simulator

**Command / flow that triggers the bug**
```
# e.g. CR 1, then BYE, then BULK 10
```

**Expected behavior**
What Wireshark should show / what the log should say.

**Actual behavior**
What actually happens (paste the relevant log lines).

**Wireshark observation**
If pcap is involved: which dissector shows the problem? (S1AP / SIP / Diameter / GTPv2 / PFCP)

**Environment**
- OS:
- Compiler + version:
- CMake version:
- Built with Docker? (yes/no)
