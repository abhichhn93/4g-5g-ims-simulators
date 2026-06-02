# IMS/VoLTE — Beginner Guide: How Does a 4G Phone Call Actually Work?

> No jargon. If you understand WhatsApp calls, you'll understand this.

---

## The Short Answer

A 4G phone call (VoLTE) is basically a WhatsApp call — but standardized, secured, and carrier-grade.

Voice goes over the internet (IP). But instead of WhatsApp's servers, it uses a carrier-run system called **IMS** — IP Multimedia Subsystem.

---

## Why VoLTE? Why Not Just Use WhatsApp?

Old networks (2G/3G) had separate circuits for calls. 4G only has data. So for calls on 4G, carriers needed a standard — something that works between Airtel and Vodafone, between India and UK.

That standard is VoLTE (Voice over LTE) + IMS.

---

## The Cast of Characters

| Name | What it is | Plain English |
|------|-----------|---------------|
| **P-CSCF** | Proxy-Call Session Control Function | The "security guard" — first thing your phone calls when setting up a call |
| **S-CSCF** | Serving-CSCF | The "main exchange" — routes the call, checks your services |
| **MTAS** | Multimedia Telephony Application Server | The "features manager" — handles call waiting, forwarding, barring |
| **IMS-HSS** | IMS Home Subscriber Server | The "database" — stores who you are and what services you have |
| **MRFC/MRFP** | Media Resource Function | The "conference mixer" — puts 3 people in one call |

---

## Step by Step — Making a VoLTE Call

### Before the call: Registration (happens when your 4G turns on)

After your phone does its 4G attach (gets an IP from the network), it registers with IMS:

1. Phone says: *"Hi P-CSCF, I'm +919000000001, I'm reachable at IP 10.0.0.1"*
2. P-CSCF forwards to S-CSCF
3. S-CSCF checks HSS: *"Is this user valid? What services do they have?"*
4. HSS says: *"Yes, valid. They have call waiting, call forwarding, OIP enabled"*
5. S-CSCF notifies MTAS: *"This user is now online"*
6. Phone gets: *"Registered. You're reachable."*

Now your phone can receive calls.

### Making a call: SIP INVITE

You call +919000000002:

1. Your phone sends **SIP INVITE** to P-CSCF
2. P-CSCF → S-CSCF → MTAS: *"Is this call allowed? Is the caller barred?"*
3. MTAS checks: Not barred, CLI allowed → proceed
4. MTAS routes to the called person's network
5. Called phone rings → **SIP 180 Ringing**
6. Called person answers → **SIP 200 OK** with voice settings (codec)
7. Your phone confirms → **SIP ACK**

Now the "pipe" for voice is open. But voice still needs a fast lane...

### The voice lane: QCI=1 Bearer

After SIP 200 OK, the P-CSCF tells the network:
*"Start a dedicated fast lane (QCI=1) for this call"*

This is the link between IMS and 4G EPC:
- P-CSCF → PCRF: *"I need a QCI=1 bearer"*
- PCRF → P-GW: *"Create a dedicated bearer"*
- P-GW → your phone: dedicated data pipe, just for voice

This bearer gets priority over YouTube, WhatsApp, everything.
Your voice always goes first. That's why calls don't stutter.

### The voice itself: RTP/AMR-WB

Voice travels as **RTP packets** (not SIP). The codec is **AMR-WB**:
- AMR = Adaptive Multi-Rate (adjusts quality based on signal)
- WB = Wideband (16kHz sampling — sounds like being in the same room)
- Rate: 12.65 kbps — very small, very efficient

---

## Conference Call

When 3 people are in a call:
- A bridge is created (MRFC/MRFP)
- Everyone's voice goes to the bridge
- Bridge sends back a "mixed" stream to each person
  - A hears B+C. B hears A+C. C hears A+B.

---

## In the Simulator

```
ims-sim> REG ALL           → registers UE-A, UE-B, UE-C
ims-sim> CALL A B          → A calls B (watch all 8 steps in color)
ims-sim> CONF              → 3-party conference
ims-sim> WAIT              → B busy, C calls B (call waiting)
ims-sim> BARR              → A tries to call UK number (barred)
```

Open `ims_capture.pcap` in Wireshark → filter `sip` → see every message.
