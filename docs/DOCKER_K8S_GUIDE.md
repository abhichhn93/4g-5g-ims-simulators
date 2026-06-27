# Docker & Kubernetes — Complete Guide
## Using your 4G / 5G / IMS Simulator codebase

> Every concept here is backed by a real file in this repo.
> You built and ran all of this — this doc explains WHY each line exists.

---

## SECTION 1 — Docker Basics

### What is Docker? (one sentence)

Docker packages your C++ binary + all its dependencies (glibc, cmake output, etc.)
into a single sealed box called an **image** that runs identically anywhere.

### Key vocabulary

| Word | What it is | Analogy |
|------|-----------|---------|
| **Image** | The blueprint — a read-only snapshot of your app | Recipe / ISO file |
| **Container** | A running instance of an image | A running VM (but lightweight) |
| **Dockerfile** | Instructions to BUILD an image | Makefile for Docker |
| **Registry** | Where images are stored (Docker Hub, ECR) | GitHub for images |
| **Layer** | Each `RUN`/`COPY` line in Dockerfile = one cached layer | Git commit |

---

### YOUR Dockerfile — 4G Simulator
**File:** `4g-simulator/Dockerfile`

```dockerfile
# ── STAGE 1: Builder ──────────────────────────────────────
FROM debian:bookworm AS builder         # start from Debian (has gcc, apt)
                                         # AS builder = name this stage

RUN apt-get update && \                 # RUN = execute inside the image
    apt-get install -y build-essential cmake && \
    rm -rf /var/lib/apt/lists/*         # clean up to reduce layer size

WORKDIR /src                            # cd /src inside the image

COPY CMakeLists.txt ./                  # COPY from your Mac → into image
COPY src ./src

RUN cmake -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --target mme_sim -j4   # compile mme_sim

# ── STAGE 2: Runtime (slim, no compiler) ──────────────────
FROM python:3.12-slim-bookworm          # fresh start — NO gcc/cmake

WORKDIR /app
COPY --from=builder /src/build/mme_sim ./mme_sim  # copy ONLY the binary
COPY docker/entrypoint.sh ./entrypoint.sh          # from stage 1
RUN chmod +x entrypoint.sh

EXPOSE 8080                             # document the port (does NOT open it)
ENTRYPOINT ["./entrypoint.sh"]          # run this when container starts
```

**Why 2 stages?**
- Stage 1 needs gcc + cmake = 800 MB
- Stage 2 ships only the binary = 130 MB
- You never ship build tools to production (security + size)

### Build and run commands

```bash
cd 4g-simulator

# BUILD the image
docker build -t mme-sim:latest .
#             ↑ name:tag        ↑ use current directory as context

# RUN a container from it
docker run --rm -p 8080:8080 mme-sim:latest
#           ↑       ↑         ↑ which image
#         delete   port map   (Mac:8080 → container:8080)
#         on exit

# LIST running containers
docker ps

# LIST all images
docker images

# LOGS of a container
docker logs <container-id>

# SHELL inside a running container
docker exec -it <container-id> sh

# STOP a container
docker stop <container-id>

# DELETE an image
docker rmi mme-sim:latest
```

### Layer cache — why rebuilds are fast

```bash
docker build -t mme-sim:latest .

# Output:
[+] Building 45s (12/12) FINISHED
 => [builder 1/6] FROM debian:bookworm          CACHED  ← already downloaded
 => [builder 2/6] RUN apt-get install...        CACHED  ← not re-run
 => [builder 5/6] COPY src ./src                ←  you changed a .cpp file
 => [builder 6/6] RUN cmake --build...          ←  re-compiles (depends on src)
```

Change a `.cpp` file → only steps AFTER `COPY src` re-run.
Change `CMakeLists.txt` → only steps AFTER `COPY CMakeLists.txt` re-run.
Order of COPY matters for cache efficiency.

---

## SECTION 2 — Docker Compose

### What is Compose? (one sentence)

Compose runs **multiple containers together** with one command, giving them a shared
network where each container can find the others by name.

### YOUR Compose file — 5G Simulator
**File:** `5g-simulator/docker-compose.yml`

```yaml
services:            # "services" = containers to run

  nrf-sim:           # Container 1: name = nrf-sim
    build:
      context: .
      target: nrf    # use "nrf" stage from Dockerfile
    image: g5-nrf:latest

  udm-sim:           # Container 2
    build:
      context: .
      target: udm
    environment:
      - NRF_HOST=nrf-sim      # ← KEY: "nrf-sim" becomes a DNS hostname!
      - UDM_SELF_HOST=udm-sim #   Docker creates DNS: nrf-sim → container IP
    depends_on:
      - nrf-sim               # start nrf-sim first (but not wait for ready)

  amf-sim:
    environment:
      - NRF_HOST=nrf-sim      # AMF finds NRF by name "nrf-sim"
      - AMF_SELF_HOST=amf-sim
      - UDM_HOST=udm-sim
    ports:
      - "38412:38412"         # Mac port 38412 → container port 38412

  gnb-sim:
    environment:
      - AMF_HOST=amf-sim      # gNB connects to AMF by name
      - REG_UES=2             # register 2 UEs
    depends_on:
      - amf-sim
```

**How DNS works in Compose:**
All services share one bridge network. Docker's internal DNS resolves
each service name → that container's IP automatically.
`NRF_HOST=nrf-sim` works because Docker DNS resolves `nrf-sim`.

### Compose commands

```bash
cd 5g-simulator

# BUILD all images + START all containers
docker compose up --build

# START in background (detached)
docker compose up -d --build

# SEE running containers
docker compose ps

# LOGS of one service
docker compose logs -f nrf-sim

# LOGS of all services
docker compose logs -f

# SHELL into a service
docker compose exec amf-sim sh

# STOP all containers
docker compose down

# STOP + delete volumes too
docker compose down -v
```

---

## SECTION 3 — Kubernetes Core Concepts

### The mental model

```
You tell K8s WHAT you want (desired state).
K8s figures out HOW to make it happen.

You:  "I want 3 copies of AMF running always."
K8s:  - Creates 3 Pods
      - Watches them
      - If one crashes → creates a new one automatically
      - If node dies   → reschedules on another node
```

### The 6 objects you must know

```
ConfigMap  → runtime config (env vars without recompile)
    │
    ▼
Pod        → one running container (smallest unit)
    │
ReplicaSet → "keep N pods alive" (created by Deployment automatically)
    │
Deployment → desired state manager (you talk to this, not ReplicaSet)
    │
Service    → stable network name in front of changing Pods
    │
Ingress    → HTTP routing from outside world into Services
```

---

## SECTION 4 — Pod

### What is a Pod?

The smallest thing K8s manages. Wraps one container (usually).
Gets its own IP. Dies and gets a NEW IP on restart.

**YOU NEVER CREATE PODS DIRECTLY** — you create Deployments.

### What a Pod YAML looks like (simplified)

```yaml
apiVersion: v1
kind: Pod
metadata:
  name: nrf-pod
spec:
  containers:
    - name: nrf-sim
      image: g5-nrf:latest
      ports:
        - containerPort: 29510
```

### Commands

```bash
kubectl get pods                     # list all pods
kubectl get pods -w                  # watch live (updates as things change)
kubectl describe pod <name>          # full detail — events, resource usage
kubectl logs <pod-name>              # pod output
kubectl logs -f <pod-name>           # follow live
kubectl exec -it <pod-name> -- sh    # shell inside the pod
kubectl delete pod <name>            # delete it (Deployment recreates it)
```

**What `kubectl describe pod` shows:**
```
Name:         g5-nrf-6d8f9-abc12
Namespace:    default
Node:         minikube/192.168.49.2
IP:           10.244.0.5          ← Pod IP (CHANGES on restart)
Status:       Running

Containers:
  nrf-sim:
    Image:    g5-nrf:latest
    Port:     29510/TCP
    State:    Running

Events:
  Scheduled → Pulled image → Created container → Started container
```

---

## SECTION 5 — Deployment

### What is a Deployment?

Describes your DESIRED STATE: "I want 2 copies of this Pod, always."
Creates and manages a ReplicaSet which watches the actual count.

### YOUR Deployment — 4G Simulator
**File:** `4g-simulator/k8s/deployment.yaml`

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: mme-sim          # name of this Deployment
  labels:
    app: mme-sim         # labels on the Deployment itself

spec:
  replicas: 2            # DESIRED STATE: always keep 2 Pods

  selector:
    matchLabels:
      app: mme-sim       # manage Pods that have THIS label
                         # must match template.metadata.labels below

  template:              # blueprint for the Pods it creates
    metadata:
      labels:
        app: mme-sim     # every Pod gets this label (selector must match)
    spec:
      containers:
        - name: mme-sim
          image: mme-sim:latest
          imagePullPolicy: Never   # use local minikube image
          ports:
            - containerPort: 8080
```

**The selector ↔ labels connection:**
- `selector.matchLabels: app: mme-sim` — Deployment watches Pods with this label
- `template.metadata.labels: app: mme-sim` — every Pod it creates gets this label
- They MUST match or K8s rejects the Deployment

### YOUR Deployment — 5G NRF (with probes)
**File:** `5g-simulator/k8s/nrf-deployment.yaml`

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: g5-nrf
spec:
  replicas: 1
  selector:
    matchLabels:
      app: g5-nrf
  template:
    metadata:
      labels:
        app: g5-nrf
    spec:
      containers:
        - name: nrf-sim
          image: g5-nrf:latest
          imagePullPolicy: Never
          ports:
            - containerPort: 29510

          readinessProbe:           # "Am I ready to receive traffic?"
            tcpSocket:
              port: 29510           # try to open TCP connection to this port
            initialDelaySeconds: 1  # wait 1s before first probe
            periodSeconds: 5        # probe every 5s
            # FAIL = remove from Service load balancer (no traffic)

          livenessProbe:            # "Am I still alive?"
            tcpSocket:
              port: 29510
            initialDelaySeconds: 5
            periodSeconds: 10
            # FAIL = kill + restart the container
```

**Readiness vs Liveness:**
| Probe | On failure | Use case |
|---|---|---|
| readiness | Remove from Service (no new traffic) | Starting up, temporarily overloaded |
| liveness | Kill + restart container | Deadlocked, hung, infinite loop |

### Deployment commands

```bash
kubectl apply -f k8s/deployment.yaml   # create or update

kubectl get deployments                # list deployments
kubectl describe deployment mme-sim    # full detail

# SELF-HEALING DEMO:
kubectl delete pod <pod-name>          # delete one pod
kubectl get pods -w                    # watch it get recreated automatically

# SCALING:
kubectl scale deployment mme-sim --replicas=4
kubectl get pods                       # now 4 pods

# ROLLING UPDATE (zero-downtime):
kubectl set image deployment/mme-sim mme-sim=mme-sim:v2
kubectl rollout status deployment/mme-sim   # watch progress
kubectl rollout undo deployment/mme-sim     # rollback if broken
```

---

## SECTION 6 — Service

### What is a Service?

Pods die and get NEW IPs. A Service gives a STABLE IP + DNS name
that never changes, and load-balances across all matching Pods.

```
PROBLEM:
  AMF connects to NRF Pod at 10.244.0.5
  NRF Pod crashes → restarts → new IP 10.244.0.8
  AMF's connection breaks!

SOLUTION (Service):
  AMF connects to g5-nrf-svc (DNS name, stable)
  Service IP: 10.96.45.123 (never changes)
  kube-proxy routes: 10.96.45.123 → current NRF Pod IP (updates automatically)
```

### Three Service types

```
ClusterIP  (default)  → only reachable INSIDE the cluster
                         used for: pod-to-pod (AMF → NRF, AMF → UDM)

NodePort              → also exposed on every node at a high port
                         used for: laptop → cluster (dev/test)
                         your Mac → minikube_ip:30080

LoadBalancer          → cloud only (AWS ELB, GCP LB)
                         used for: internet traffic → production service
```

### YOUR Service — 4G Simulator
**File:** `4g-simulator/k8s/service.yaml`

```yaml
apiVersion: v1
kind: Service
metadata:
  name: mme-sim-svc        # DNS name inside cluster: mme-sim-svc

spec:
  type: NodePort            # also expose on host port (for your laptop)

  selector:
    app: mme-sim            # route to Pods with this label

  ports:
    - port: 8080            # Service listens on this port (inside cluster)
      targetPort: 8080      # forward to this port on the Pod
      nodePort: 30080       # your Mac can reach: minikube_ip:30080
```

### YOUR Service — 5G UDM (ClusterIP — internal only)
**File:** `5g-simulator/k8s/udm-service.yaml`

```yaml
apiVersion: v1
kind: Service
metadata:
  name: g5-udm-svc         # DNS: g5-udm-svc (inside cluster only)

spec:
  selector:
    app: g5-udm
  ports:
    - port: 29503
      targetPort: 29503
  # No type: = defaults to ClusterIP (internal only)
  # AMF finds UDM via: NRF_HOST=g5-udm-svc
```

### Service commands

```bash
kubectl apply -f k8s/service.yaml

kubectl get services                    # list services + their IPs
kubectl describe service mme-sim-svc   # shows Endpoints (pod IPs behind it)

# ACCESS via NodePort (from your Mac):
minikube service mme-sim-svc --url      # prints: http://192.168.49.2:30080
curl $(minikube service mme-sim-svc --url)/mme_capture.pcap
```

**What `kubectl describe service` shows:**
```
Name:          g5-amf-svc
Type:          NodePort
IP:            10.96.112.56          ← stable ClusterIP (never changes)
Port:          38412/TCP
NodePort:      30412/TCP             ← reach from your Mac
Endpoints:     10.244.0.8:38412,     ← actual Pod IPs (changes on restart)
               10.244.0.9:38412      ← Service updates this automatically
```

---

## SECTION 7 — ConfigMap

### What is a ConfigMap?

Key-value store injected as environment variables into Pods.
Change config WITHOUT rebuilding the image.

### YOUR ConfigMap — 5G Simulator
**File:** `5g-simulator/k8s/plmn-configmap.yaml`

```yaml
apiVersion: v1
kind: ConfigMap
metadata:
  name: g5-plmn-config

data:
  PLMN_MCC: "404"      # MCC = Mobile Country Code (India = 404)
  PLMN_MNC: "10"       # MNC = Mobile Network Code
```

**How Pods use it** (from `5g-simulator/k8s/amf-deployment.yaml`):
```yaml
spec:
  containers:
    - name: amf-sim
      envFrom:
        - configMapRef:
            name: g5-plmn-config   # inject ALL keys as env vars
      env:
        - name: NRF_HOST
          value: g5-nrf-svc        # also add individual vars
```

Inside the container: `$PLMN_MCC=404`, `$PLMN_MNC=10` automatically.

### Change config without recompile

```bash
# 1. Edit the ConfigMap
kubectl edit configmap g5-plmn-config
# (editor opens, change PLMN_MCC to 310)

# 2. Rolling restart to pick it up
kubectl rollout restart deployment/g5-amf

# 3. Verify new value
kubectl logs deployment/g5-amf | grep PLMN
```

### ConfigMap vs Secret

| | ConfigMap | Secret |
|---|---|---|
| Content | Plain text (MCC, MNC, hostnames) | Base64-encoded sensitive data |
| Use for | Config, URLs, feature flags | Passwords, TLS certs, API keys |
| Viewable | Yes (`kubectl get cm -o yaml`) | Base64 only (not real encryption) |

---

## SECTION 8 — Full K8s Workflow (Run Your 5G Sim in K8s)

```bash
# ── Prerequisites ─────────────────────────────────────────────
minikube start
eval $(minikube docker-env)    # point docker at minikube's daemon

# ── Build images INTO minikube ────────────────────────────────
cd 5g-simulator
for t in nrf udm amf smf upf gnb; do
  docker build -t g5-$t:latest --target $t .
done

# ── Apply in order ────────────────────────────────────────────
kubectl apply -f k8s/plmn-configmap.yaml   # ConfigMap first

kubectl apply -f k8s/nrf-deployment.yaml   # NRF (others depend on it)
kubectl apply -f k8s/nrf-service.yaml

kubectl apply -f k8s/udm-deployment.yaml
kubectl apply -f k8s/udm-service.yaml

kubectl apply -f k8s/amf-deployment.yaml
kubectl apply -f k8s/amf-service.yaml

kubectl apply -f k8s/gnb-deployment.yaml   # gNB last

# ── Watch pods come up ────────────────────────────────────────
kubectl get pods -w

# ── Check logs ────────────────────────────────────────────────
kubectl logs deployment/g5-nrf
kubectl logs deployment/g5-amf
kubectl logs -l app=g5-amf --prefix=true   # all AMF replicas at once

# ── Self-healing demo ─────────────────────────────────────────
kubectl delete pod <any-pod-name>
kubectl get pods -w    # watch it come back

# ── Scale AMF to 3 replicas ───────────────────────────────────
kubectl scale deployment g5-amf --replicas=3
kubectl get pods

# ── Tear down ────────────────────────────────────────────────
kubectl delete -f k8s/
minikube stop
```

---

## SECTION 9 — Helm Charts (What Interviewers Ask About)

### What is Helm?

Helm is the **package manager for Kubernetes** — like `apt` for Ubuntu or `brew` for macOS.

**Problem without Helm:**
Your 5G sim has 10 YAML files. To deploy it you run 10 `kubectl apply` commands.
To change the MCC, you edit the ConfigMap. To change the AMF image version,
you edit amf-deployment.yaml. Every environment (dev/staging/prod) needs
different values in the same YAML files. You end up copy-pasting YAMLs
and changing values manually — error-prone.

**Solution with Helm:**
Package all your K8s YAMLs into one **chart**. Use **templates** with variables.
One command deploys everything. One `values.yaml` file controls all settings.

### Helm vocabulary

| Term | What it is | Your analogy |
|------|-----------|---|
| **Chart** | Package of K8s templates + default values | npm package |
| **Values** | Variables that customize the chart | `.env` file |
| **Release** | One deployed instance of a chart | Running container |
| **Repository** | Where charts are stored | npm registry |
| **Template** | YAML file with `{{ .Values.xxx }}` placeholders | Jinja template |

### What your 5G K8s files look like as a Helm chart

**Without Helm** (what you have now):
```yaml
# k8s/plmn-configmap.yaml — hardcoded
data:
  PLMN_MCC: "404"
  PLMN_MNC: "10"
```

**With Helm** (templates + values):
```yaml
# helm/5g-sim/templates/plmn-configmap.yaml
data:
  PLMN_MCC: "{{ .Values.plmn.mcc }}"
  PLMN_MNC: "{{ .Values.plmn.mnc }}"
```

```yaml
# helm/5g-sim/values.yaml (default values)
plmn:
  mcc: "404"
  mnc: "10"
amf:
  replicas: 2
  image: g5-amf:latest
nrf:
  replicas: 1
```

**Deploy with Helm:**
```bash
# Install (first time)
helm install my-5g-sim ./helm/5g-sim

# Install with custom values (override defaults)
helm install my-5g-sim ./helm/5g-sim --set plmn.mcc=310,plmn.mnc=260

# Install with a values file
helm install my-5g-sim ./helm/5g-sim -f values-production.yaml

# Upgrade (after changes)
helm upgrade my-5g-sim ./helm/5g-sim

# List releases
helm list

# Rollback
helm rollback my-5g-sim 1

# Uninstall
helm uninstall my-5g-sim
```

### Chart structure

```
helm/5g-sim/
├── Chart.yaml          # metadata: name, version, description
├── values.yaml         # default values
└── templates/
    ├── plmn-configmap.yaml
    ├── nrf-deployment.yaml
    ├── nrf-service.yaml
    ├── amf-deployment.yaml
    ├── amf-service.yaml
    ├── udm-deployment.yaml
    └── udm-service.yaml
```

### Chart.yaml example

```yaml
apiVersion: v2
name: 5g-sim
description: 5G Core Simulator (AMF, UDM, NRF, SMF, UPF, gNB)
version: 1.0.0
appVersion: "1.0"
```

### Why interviewers ask about Helm

In production telecom (Ericsson, Nokia, Radisys):
- Different customers have different MCC/MNC
- Different environments (lab/staging/prod) have different replica counts
- Helm lets you manage one chart, multiple deployments, different values
- Real O-RAN deployments use Helm charts (see O-RAN Software Community charts)

---

## SECTION 10 — Interview Quick Reference

### Key differences you must know

**Pod vs Deployment:**
- Pod = one instance. Delete it → gone.
- Deployment = desired state. Delete a Pod → Deployment recreates it.

**Service types:**
- ClusterIP = pod-to-pod inside cluster (NRF ↔ AMF ↔ UDM)
- NodePort = your laptop → cluster (dev testing with minikube)
- LoadBalancer = internet → production (AWS ELB)

**ConfigMap vs Secret:**
- ConfigMap = plain text config (MCC/MNC, hostnames)
- Secret = sensitive data (passwords, certs) in base64

**Readiness vs Liveness probe:**
- Readiness fail = no traffic sent to this Pod (but not restarted)
- Liveness fail = Pod killed and restarted

**Docker vs K8s:**
- Docker = runs ONE container on ONE machine
- K8s = runs MANY containers across MANY machines, self-healing

### One-line answers

| Question | Answer |
|---|---|
| What is a Pod? | Smallest K8s unit — wraps one container, gets its own IP |
| What is a ReplicaSet? | Watches pod count, recreates if one dies (created by Deployment) |
| Why Services? | Pod IPs change on restart — Service gives stable DNS + IP |
| What is Helm? | Package manager for K8s — templates + values for multi-env deploys |
| What is imagePullPolicy: Never? | Use local image, don't pull from Docker Hub (for minikube) |
| What is kube-proxy? | Runs on every node, programs iptables to route Service IP → Pod IP |
| Rolling update? | New ReplicaSet scales up, old one scales down — zero downtime |
| What is a Namespace? | Virtual cluster inside K8s — isolates resources (dev/prod) |

### What you can say in the interview

> "I containerized the 4G EPC simulator with a multi-stage Dockerfile — stage 1 compiles the C++ binary with gcc and cmake, stage 2 copies only the binary into a python:slim image at 130 MB instead of 800 MB. For the 5G core, I used Docker Compose with 6 services (NRF, UDM, AMF, SMF, UPF, gNB) on a shared bridge network using service names as DNS. In Kubernetes, I deployed the 5G core with Deployments, ClusterIP Services for pod-to-pod (AMF→NRF, AMF→UDM), NodePort for external access in minikube, and a ConfigMap for the PLMN MCC/MNC so I can change the network identity without recompiling. The NRF and AMF deployments have readiness and liveness TCP probes. I verified self-healing by deleting a Pod and watching the Deployment recreate it, and demonstrated horizontal scaling with `kubectl scale`."

---

## SECTION 11 — Commands Cheat Sheet

```bash
# ── DOCKER ──────────────────────────────────────────────────
docker build -t name:tag .           # build image
docker images                        # list images
docker run --rm -p host:cont image   # run container
docker ps                            # running containers
docker ps -a                         # all containers
docker logs <id>                     # logs
docker logs -f <id>                  # follow logs
docker exec -it <id> sh              # shell inside
docker stop <id>                     # stop
docker rm <id>                       # delete container
docker rmi <name>                    # delete image

# ── DOCKER COMPOSE ───────────────────────────────────────────
docker compose up --build            # build + start all
docker compose up -d                 # background
docker compose ps                    # status
docker compose logs -f <service>     # follow logs
docker compose exec <svc> sh         # shell into service
docker compose down                  # stop + delete containers

# ── KUBECTL — BASICS ─────────────────────────────────────────
kubectl get pods                     # list pods
kubectl get pods -w                  # watch live
kubectl get pods -o wide             # show node + IP columns
kubectl describe pod <name>          # full detail + events
kubectl logs <pod>                   # logs
kubectl logs -f <pod>                # follow
kubectl logs -l app=g5-amf --prefix  # all pods with label
kubectl exec -it <pod> -- sh         # shell

# ── KUBECTL — RESOURCES ──────────────────────────────────────
kubectl get deployments
kubectl get replicasets
kubectl get services
kubectl get configmaps
kubectl get all                      # everything at once

# ── KUBECTL — ACTIONS ────────────────────────────────────────
kubectl apply -f file.yaml           # create/update resource
kubectl delete -f file.yaml          # delete resource
kubectl delete pod <name>            # delete pod (Deployment recreates)
kubectl scale deployment <name> --replicas=4
kubectl rollout restart deployment/<name>
kubectl rollout status deployment/<name>
kubectl rollout undo deployment/<name>
kubectl edit deployment <name>       # edit live in editor

# ── HELM ─────────────────────────────────────────────────────
helm install <release> ./chart       # install chart
helm install <release> ./chart --set key=val   # override value
helm upgrade <release> ./chart       # upgrade
helm list                            # list releases
helm rollback <release> 1            # rollback to version 1
helm uninstall <release>             # remove

# ── MINIKUBE ─────────────────────────────────────────────────
minikube start
minikube stop
eval $(minikube docker-env)          # use minikube's Docker daemon
minikube service <svc> --url         # get NodePort URL
minikube dashboard                   # browser UI
minikube ip                          # get minikube IP
```
