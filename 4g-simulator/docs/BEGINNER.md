# Level 1 — Beginner Guide: What Actually Happens When You Turn Airplane Mode Off?

> No jargon. No 3GPP numbers. Just the real story.

---

## Your Phone Has Been Lying to You

You turn airplane mode off. Two seconds later, you have signal, data, and WhatsApp messages.

What actually happened in those two seconds?

---

## The Cast of Characters (in plain English)

| What we call it | What it actually is |
|----------------|-------------------|
| **UE** (User Equipment) | Your phone |
| **eNB** (eNodeB) | The cell tower on the roof of a building |
| **MME** | The "receptionist" of the 4G network — checks your ID |
| **HSS** | The "database" — stores your SIM card secrets |
| **S-GW** | A middleman router — connects your phone to the internet highway |
| **P-GW** | The "internet door" — gives your phone an IP address |
| **PCRF** | The "traffic cop" — decides your data speed |

---

## Step by Step — What Happens in Those 2 Seconds

### Step 1: "Hello, I'm here!" (Attach Request)
Your phone broadcasts: *"I want to connect. Here's my IMSI."*

IMSI = your SIM card's unique ID number. It's like your passport.

The nearest cell tower (eNB) hears this and passes it to the MME.

### Step 2: "Let me check your ID" (Authentication)
The MME doesn't trust your phone yet. It asks the HSS:
*"This IMSI says it belongs here. Do you agree? Send me a challenge."*

The HSS generates a random number (RAND) and sends it back.

### Step 3: The Secret Handshake (Milenage Algorithm)
The MME sends the RAND to your phone.

Your phone has a SIM card with a secret key (Ki). So does the HSS.
Both run the same math (called Milenage) on Ki + RAND.

- Your phone computes: RES
- The HSS computed: XRES

If RES == XRES → You have the real SIM. Welcome.

This is EPS-AKA (Authentication and Key Agreement). It proves two things:
1. The network is real (not a fake tower trying to spy on you)
2. Your phone is real (not someone who copied your number)

### Step 4: "Here's your locker" (Session Creation)
Now the MME tells the gateway system: *"Give this user an IP address and a data pipe."*

S-GW and P-GW talk to each other and create a "bearer" — think of it as a dedicated pipe for your data.

The P-GW gives your phone an IP address (like 10.0.0.1).

### Step 5: "What speed is this user allowed?" (PCRF Policy)
The P-GW asks the PCRF: *"What plan is this user on?"*

PCRF checks → returns QCI=9 (standard internet data, not HD voice).

Think of QCI like lanes on a highway. QCI=9 is the normal lane.

### Step 6: "You're in. Here's your key." (Attach Complete)
The MME sends all the settings to the eNB and your phone.
Your phone confirms → **REGISTERED**.

Your phone now has:
- An IP address
- A data pipe (called a bearer)
- Full 4G internet access

**Total time: ~50-200ms.** That's your 2 seconds, done.

---

## How This Simulator Shows It

When you type `CR 1`, you watch all of this happen live:

```
  +============================================================+
  | STEP 1/8  ATTACH REQUEST              [T-47329]            |
  | UE -----------------------------------------> eNB -> MME   |
  | [UE: DEREGISTERED -> REG_PENDING]                          |
  |   IMSI: 404010000000001                                    |
  |   TAI:  MCC=404 MNC=01 TAC=0x0001                         |
  +------------------------------------------------------------+
  | > Next: MME sends Authentication-Info-Req to HSS           |
  +============================================================+
```

Each color = a different node (eNB=green, MME=blue, HSS=yellow, etc.)

---

## For VoLTE (Voice Calls)

After Step 6, your phone is on 4G data. But phone calls?

Old networks used a separate system for calls. 4G is smarter:
Voice goes over the internet (SIP protocol, like Skype — but standardized).

This is VoLTE. Run `./mme_ims` (in the `../ims-simulator/` sibling project) to see the voice part:
- SIP REGISTER = your phone tells the IMS network "I'm reachable for calls"
- SIP INVITE = someone calls you
- The network creates a special fast lane (QCI=1) just for your voice
