# Secure Networking — Interview Defense Guide

> **How to read this:** Read top to bottom once today.
> The "What to say" boxes are your interview answers — short, confident, no extra detail.
> The "Simple explanation" sections are so YOU understand what you're saying.

---

## The Honest Picture First

Before anything else — what you actually built vs what you know from work:

| What | Where | Status |
|---|---|---|
| IPSec IKEv2 tunnel with certificates | `ipsec-lab/` on your Mac | **Actually built, runs in Docker, PCAP captured** |
| TLS on SIP channels (VoLTE) | LG Soft — production | Real experience, you saw it work on real handsets |
| TLS/IPSec on 5G control plane | Samsung — production team | You were there, saw the design, not the sole owner |
| TLS in 4G/5G/IMS simulator code | `4g-5g-ims-simulators/` | **Code uses plain TCP** — TLS is mentioned in comments only |

> **Interview rule:** For the simulator, say "the architecture is designed for TLS — the socket layer is abstracted for it — but in the demo environment it runs over plain TCP for simplicity." That is true (the socket_wrapper.h is an abstraction layer) and draws no follow-up.

---

## Part 1 — What is TLS and Where Does It Sit

### Simple explanation (like you're explaining to a friend)

Imagine you and a friend want to pass notes in class. Anyone can read your notes.
TLS is like putting your note in a sealed envelope that only your friend can open.

The sealed envelope has two things:
1. **Verification** — a stamp proving the note is really from you (certificate)
2. **Encryption** — only your friend can read what's inside (AES encryption)

### Where it sits in the network stack

```
Your Application (SIP message, S1AP packet, HTTPS request)
        |
        |   ← TLS sits here. It wraps your data before sending.
        |      The application doesn't know about encryption.
        |      It just calls SSL_write() instead of send().
        |
       TLS  (handshake → certificates → session key → encrypted data)
        |
       TCP  (reliable delivery, port 443 or 5061)
        |
       IP   (routing)
        |
   Physical network
```

### In plain language for interview

> "TLS sits between the application and TCP. The application writes data, TLS encrypts it using a session key that was negotiated during the handshake, and TCP carries the encrypted bytes. The receiving side decrypts before passing to the application. Neither side can be faked because both sides show a certificate signed by a trusted CA."

---

## Part 2 — What is IPSec and Where Does It Sit

### Simple explanation

TLS protects one application's data (like one SIP call, one web page).
IPSec protects **everything** going between two machines — every packet, every application, at the network layer itself.

Think of it this way:
- TLS = sealing one letter
- IPSec = sealing the entire mail truck

### Where it sits — this is the KEY thing to know

```
Application (SIP, HTTP, anything)
        |
       TCP/UDP
        |
       IP header  ← IPSec in TRANSPORT mode protects from here down
        |
  ┌─────────────────────────┐
  │  ESP header             │  ← IPSec adds this wrapper
  │  [ Original IP packet ] │  ← your original packet is encrypted inside
  │  ESP trailer + auth tag │  ← integrity check (nobody tampered with it)
  └─────────────────────────┘
        |
   Physical network
```

### Tunnel mode vs Transport mode (they will ask this)

```
TRANSPORT MODE — protects payload, IP header stays visible:
  [ Original IP header | ESP | TCP | Data | ESP-auth ]
  Peer A IP and Peer B IP are visible to the network.
  Used between two endpoints that are the final destination.

TUNNEL MODE — hides EVERYTHING, wraps in new IP header:
  [ NEW outer IP | ESP | Original IP | TCP | Data | ESP-auth ]
  Used for site-to-site VPN (what you built in the lab).
  The original IPs are hidden inside the encrypted ESP.
```

### In plain language for interview

> "IPSec sits at the IP layer, below TCP. It protects every packet between two endpoints — not just one application's traffic. In our lab we used tunnel mode, which completely hides the original packet inside an ESP wrapper with a new outer IP header. This is how site-to-site VPNs work — traffic between two sites looks like it's just between the two gateway IPs, the internal IPs are encrypted inside."

---

## Part 3 — What You Actually Built (the ipsec-lab)

### What it is

Two Linux boxes (Docker containers) connected over a simulated internet.
They establish an IPSec tunnel between themselves using certificates to prove identity.
Once the tunnel is up — any traffic between their internal networks (10.0.0.0/24 and 10.0.1.0/24) is automatically encrypted.

### The setup

```
Your Mac
   |
   |── Docker network (172.20.0.0/24) ── "the internet"
        |                   |
     site-a               site-b
   172.20.0.10          172.20.0.20
   "LAN: 10.0.0.x"      "LAN: 10.0.1.x"
        |                   |
        └─── IKEv2 handshake ──→ negotiate keys
        └─── ESP tunnel (encrypted) ──→ protect traffic
```

Files:
- `docker-compose.yml` — defines the two containers and the network
- `configs/siteA/swanctl.conf` — IKEv2 config (algorithms, certificates, traffic selectors)
- `pki/ca/ca.crt` — the CA (Certificate Authority) you created
- `pki/siteA/siteA.crt`, `pki/siteB/siteB.crt` — identities for each site
- `capture/ipsec_tunnel.pcap` — the captured packets (open in Wireshark)

### The two phases of IPSec (this comes up in every interview)

**Phase 1 — IKE_SA (the handshake, like a TLS handshake)**

```
site-a                                            site-b
  |                                                  |
  |──── IKE_SA_INIT: "I support AES256-SHA256" ────▶|
  |◀─── IKE_SA_INIT: "Me too, here's my nonce" ─────|
  |                                                  |
  |──── IKE_AUTH: "Here's my certificate" ─────────▶|
  |     (cert signed by shared CA)                   |
  |◀─── IKE_AUTH: "Here's mine. I trust you." ───────|
  |                                                  |
  Both sides now have a shared secret key.
  This channel is called the IKE_SA.
```

**Phase 2 — CHILD_SA (the actual data tunnel)**

```
site-a                                            site-b
  |                                                  |
  |──── "Create tunnel for 10.0.0.0/24→10.0.1.0/24"▶|
  |◀─── "Agreed. Here's the ESP SA." ────────────────|
  |                                                  |
  Now ALL packets between those subnets are encrypted
  using the ESP SA (Security Association).
```

### The config — what each line means

From `configs/siteA/swanctl.conf`:

```
version = 2                     → use IKEv2 (newer, simpler than IKEv1)

proposals = aes256-sha256-modp2048
  aes256     → encrypt the IKE channel with AES 256-bit
  sha256     → verify integrity with SHA-256 hash
  modp2048   → Diffie-Hellman group 14 for key exchange
               (Diffie-Hellman = how two sides agree on a key without
                anyone in the middle seeing what it is)

auth = pubkey                   → use certificates, not a password (PSK)
certs = siteA.crt               → our identity proof, signed by the CA
id = 172.20.0.10                → must match the IP in our certificate's SAN field

esp_proposals = aes256gcm128    → encrypt data packets with AES-GCM
                                   GCM = does encryption AND integrity together
                                   (one step instead of two)

mode = tunnel                   → wrap original packet completely (site-to-site VPN)

local_ts  = 10.0.0.0/24        → traffic from our LAN goes into the tunnel
remote_ts = 10.0.1.0/24        → traffic to their LAN goes into the tunnel

start_action = start            → automatically bring up the tunnel when daemon starts
```

### How to run it

```bash
cd /Users/abhichauhan/Desktop/cpp-interview-prep/ipsec-lab

# Start both containers (they auto-negotiate the tunnel)
docker compose up

# In another terminal — check tunnel status on site-a
docker exec site-a swanctl --list-sas

# You should see something like:
# site-a-to-b: #1, ESTABLISHED, IKEv2, ...
#   tunnel: #1, reqid 1, INSTALLED, TUNNEL, ESP:AES_GCM_16_256

# To see the raw packets
# (the PCAP in capture/ was captured like this:)
docker exec site-a tcpdump -i eth0 -w /capture/ipsec_tunnel.pcap
```

---

## Part 4 — TLS vs IPSec Side by Side

```
                    TLS                          IPSec
─────────────────────────────────────────────────────────────
Sits at:         Application ↔ TCP            IP layer
Protects:        One app's data only          ALL traffic
Setup:           Per application              Per network path
Who configures:  Developer (in code)          Network/System admin
Visible to app:  Yes (SSL_write instead of    No (app doesn't know)
                 write)
Used for:        HTTPS, SIP-TLS, gRPC         VPN, site-to-site,
                 (5061)                       5G N2/N3 transport
Example you      CBRS ↔ SAS (HTTPS),         ipsec-lab (Docker),
can mention:     SIP over TLS at LG Soft      Samsung N2/N3 design
```

---

## Part 5 — What You Can Say for Each Resume Line

### Line 1: "TLS-secured inter-node communication" (4G/5G simulator)

> "The simulator's socket layer is abstracted — it's structured so TLS can be dropped in at the socket layer without changing the protocol logic above it. In the demo environment we ran plain TCP for simplicity, but the architecture mirrors what the real nodes use — for example S1-MME and S1-U in production run over SCTP with IPSec protecting the transport. My hands-on with TLS is from the CBRS work and from LG Soft where SIP calls ran over TLS port 5061."

Why this works: True, not embellished, and redirects to things you can actually defend.

---

### Line 2: "IPSec-protected transport channels"

> "I set up a real IKEv2 site-to-site IPSec tunnel using strongSwan in Docker. Two containers, each with their own X.509 certificate signed by a CA I generated. Phase 1 negotiates AES-256-SHA-256 for the IKE control channel. Phase 2 sets up a CHILD_SA with AES-GCM for the data — tunnel mode, so the original IP headers are hidden inside the ESP wrapper. I captured the handshake in Wireshark. In the 5G context, N2 (NGAP between gNB and AMF) and N3 (GTP-U) are similarly IPSec-protected at the transport level in production."

Why this works: You built exactly this. Every word is defensible. You end by connecting it to 5G.

---

### Line 3: "TLS-secured SAS communication with certificate validation" (CBRS)

> "The CBRS Domain Proxy communicates with the SAS over HTTPS — TLS with mutual certificate validation. The SAS verifies our certificate (the proxy acts like a client), and we verify the SAS's certificate against a trusted CA. This is the standard for CBRS — it's defined in the WInnForum spec. The simulator uses plain TCP + JSON for development simplicity, but the certificate structure and the HTTPS interface are part of the design."

Why this works: You're honest that the sim uses TCP, but explain why and what the real thing does.

---

### Line 4: "TLS-secured N2/N3 connections" (Samsung)

> "At Samsung, the AMF and gNB communicate over N2 (NGAP over SCTP) and the transport was IPSec-protected. I worked on the AMF and MME simulator side — the connection handling, session management, the protocol stack. The actual TLS/IPSec config was handled at the infrastructure layer, but I understand how it fits: N2 is the control plane (NGAP messages for registration, handover), N3 is the user plane (GTP-U encapsulated packets), and both need to be secured when running over an untrusted backhaul."

Why this works: Honest about your role, but shows you understand the architecture.

---

## Part 6 — Short Answers to Hard Questions

**Q: How does a TLS handshake work?**

> "Client says hello and lists the cipher suites it supports. Server picks one and sends its certificate. Client verifies the certificate is signed by a trusted CA. Both sides run Diffie-Hellman to agree on a session key — this is the key exchange. From then on, everything is encrypted with that session key. The certificate proves who the server is, DH gives you the key, AES does the encryption."

---

**Q: What is a certificate and why do you need a CA?**

> "A certificate is a file with two things: a public key and an identity (IP address or domain name). The CA signs it with its own private key. When site-B connects to site-A, site-A shows its certificate. Site-B checks: is this cert signed by a CA I trust? If yes, I believe this is really site-A. Without a CA anyone could generate a fake certificate saying they're site-A."

---

**Q: What is Diffie-Hellman / modp2048?**

> "It's a way for two sides to agree on a secret key without ever sending the key across the network. Both sides send a mathematical value (not the key itself). If you intercept both values you still cannot compute the key — that's the magic of DH. modp2048 means we use a 2048-bit prime number for the math. Bigger number = harder to break."

---

**Q: What is the difference between IKE Phase 1 and Phase 2?**

> "Phase 1 is the handshake — like a TLS handshake. The two sides authenticate each other using certificates, agree on algorithms, and establish an encrypted control channel called the IKE SA. Phase 2 uses that secure channel to negotiate the actual data tunnel — the CHILD SA or ESP SA. Phase 1 happens once, Phase 2 can create multiple tunnels over the same Phase 1."

---

**Q: What is AES-GCM and why use it instead of AES-CBC?**

> "GCM does two things at once — it encrypts the data AND generates an authentication tag that proves nobody tampered with it. CBC only encrypts. With CBC you need a separate HMAC step for integrity. GCM is faster because it does both in one pass. That's why ESP in modern IPSec uses AES-GCM — one step, stronger, more efficient."

---

**Q: Where exactly does IPSec run — is it in the kernel or application?**

> "The actual packet encryption is done by the Linux kernel's xfrm subsystem. The kernel has a security policy database: 'if you see a packet from 10.0.0.0/24 going to 10.0.1.0/24, encrypt it with this SA.' strongSwan (charon daemon) runs in userspace — it does the IKEv2 handshake, negotiates the keys, and writes the resulting security associations into the kernel. Once the SA is in the kernel, the kernel handles every packet transparently. The application doesn't know IPSec is there."

This is important — it's the answer most people get wrong.

---

**Q: What is NET_ADMIN in the Docker config?**

> "In Docker, containers don't have permission to change network settings by default. NET_ADMIN is a Linux capability that grants permission to create network interfaces, add IP addresses, set up routing rules, and configure the kernel's xfrm (IPSec) policies. Without it, strongSwan can't install the ESP security associations into the kernel. We also needed ip_forward=1 as a sysctl so the container would forward packets like a router."

---

## Part 7 — One Diagram to Visualize End to End

This is the full picture of what the ipsec-lab does, from your Mac to encrypted packets:

```
Your Mac  (docker compose up)
│
├─── Docker Engine
│       │
│       ├── site-a container (Alpine Linux + strongSwan)
│       │    │
│       │    ├── charon daemon (userspace — does IKEv2 handshake)
│       │    │     Reads: configs/siteA/swanctl.conf
│       │    │     Reads: pki/siteA/siteA.crt  ← our identity
│       │    │     Reads: pki/ca/ca.crt         ← who we trust
│       │    │
│       │    └── Linux kernel xfrm
│       │          ← charon writes SA here after handshake
│       │          ← kernel now encrypts every matching packet automatically
│       │
│       └── site-b container (mirror of site-a)
│
│── Docker bridge network 172.20.0.0/24 ← the "internet" between them
│
└── capture/ipsec_tunnel.pcap  ← what we captured with tcpdump
     │
     ├── IKE_SA_INIT packets  (UDP port 500) ← the handshake
     ├── IKE_AUTH packets     (UDP port 500) ← certificate exchange
     └── ESP packets          (protocol 50)  ← your encrypted data, no port visible
```

What you see in Wireshark on the pcap:
- The IKE packets show the negotiation (you can see the proposals)
- The ESP packets are just blobs — Wireshark shows "ESP (encrypted)" with no content visible
- That's proof the encryption is working — the payload is completely hidden

---

## Where the files live

```
ipsec-lab/
├── INTERVIEW_GUIDE.md          ← this file
├── docker-compose.yml          ← two containers, one bridge network
├── Dockerfile                  ← Alpine + strongSwan + tcpdump
├── configs/
│   ├── siteA/
│   │   ├── swanctl.conf        ← IKEv2 config (read every line, you understand it)
│   │   └── start.sh            ← starts charon, loads config
│   └── siteB/
│       ├── swanctl.conf        ← mirror with IPs swapped
│       └── start.sh
├── pki/
│   ├── ca/ca.crt               ← the CA you generated with openssl
│   ├── siteA/siteA.crt         ← site-a's identity
│   └── siteB/siteB.crt         ← site-b's identity
└── capture/
    └── ipsec_tunnel.pcap       ← open in Wireshark to see IKE + ESP packets
```
