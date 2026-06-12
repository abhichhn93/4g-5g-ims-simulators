# Docker + Kubernetes for the MME Simulator — Beginner Guide & Interview Q&A

> Goal: containerize `mme_sim`, run it under Docker Compose, then deploy it to a
> local Kubernetes cluster (minikube) with 2 replicas behind a Service.
> Everything below maps directly to the files in this repo:
> `Dockerfile`, `docker/entrypoint.sh`, `docker-compose.yml`, `k8s/deployment.yaml`, `k8s/service.yaml`.

---

## 0. The story in one sentence

"I containerized my 4G EPC simulator with a multi-stage Dockerfile, ran it with
Docker Compose, then deployed it to Kubernetes (minikube) as a 2-replica
Deployment behind a Service — and demoed scaling and self-healing."

That sentence, backed by you actually running the commands once, is enough for
tomorrow.

---

## 1. Docker concepts

### Image vs Container
- **Image** = a read-only template (your app + everything it needs to run).
  Think of it as a class.
- **Container** = a running instance of an image. Think of it as an object.
  You can run many containers from one image.

### Dockerfile (our actual one, explained)
Our `Dockerfile` has **two stages** — this is called a **multi-stage build**:

1. **Builder stage** (`debian:bookworm` + `build-essential` + `cmake`)
   — compiles `mme_sim` from source. This image is ~700MB but we never ship it.
2. **Runtime stage** (`python:3.12-slim-bookworm`)
   — copies *only the compiled binary* + a small entrypoint script.
   Final image is small because it has no compiler, no source code, no build tools.

**Why multi-stage?** Smaller, faster-to-pull images, and you don't ship your
compiler/source code in production.

### What does our container actually do? (`docker/entrypoint.sh`)
`mme_sim` is an interactive CLI (it waits for commands on stdin). The
entrypoint script:
1. Feeds it `CR 1` (run 1 UE attach flow) then `QUIT` — non-interactive.
2. Moves the generated `mme_capture.pcap` into `/data`.
3. Serves `/data` over HTTP on port 8080 (`python3 -m http.server`), so the
   pcap can be downloaded and the container **stays running** (important for
   the Kubernetes demo below).

### docker-compose
`docker-compose.yml` describes "how to run this app on my laptop" in one file
— build the image, map port 8080→8080, mount `./output` so the pcap lands on
your host filesystem too. `docker compose up` replaces a long `docker run ...`
command.

---

## 2. Kubernetes concepts

Docker runs containers on **one machine**. Kubernetes (K8s) runs containers
across a **cluster of machines** and answers questions like: "if a container
crashes, restart it", "if I want 5 copies, make 5 copies and keep it that
way", "give me one stable address that load-balances across all of them".

| Concept | What it is | In our project |
|---|---|---|
| **Node** | A machine (VM/physical) that runs containers | minikube = 1 node, simulating a whole cluster on your laptop |
| **Pod** | The smallest deployable unit — one or more containers that share network/storage | 1 Pod = 1 running `mme-sim` container |
| **Deployment** | A *desired state* declaration: "run N Pods of this image, keep it that way" | `k8s/deployment.yaml`, replicas: 2 |
| **ReplicaSet** | Auto-created by the Deployment. The thing that actually counts Pods and creates/deletes them to match the desired number | you never write this by hand |
| **Service** | A stable virtual IP/DNS name + load balancer in front of a changing set of Pods | `k8s/service.yaml`, NodePort 30080 |
| **Cluster** | The whole set of Nodes + the Control Plane managing them | your minikube VM |

### The Control Plane (the "brain" of the cluster)
Lives on minikube's single node too, but conceptually separate:

- **API server** — the front door. Every `kubectl` command talks to this
  (REST API). Everything else watches/talks to it.
- **etcd** — the database. Stores the *desired* state ("there should be 2
  mme-sim pods") and the *current* state.
- **Scheduler** — decides *which node* a new Pod should run on (irrelevant
  with 1 node, but it's the component that does it).
- **Controller manager** — runs the control loops, e.g. the
  ReplicaSet controller: "etcd says desired=2, actual=1 → create 1 more Pod."

### On every node
- **kubelet** — the agent that actually starts/stops containers per the API
  server's instructions, and reports Pod health back.
- **kube-proxy** — sets up networking rules so Services can route traffic to
  the right Pods.
- **Container runtime** — the thing that actually runs containers (Docker/containerd).

### The self-healing loop, concretely
1. You apply a Deployment with `replicas: 2`. This desired state is written to **etcd**.
2. The **ReplicaSet controller** (part of controller-manager) notices: desired=2, actual=0.
3. It asks the **API server** to create 2 Pods.
4. The **scheduler** assigns them to a node (only one choice on minikube).
5. **kubelet** on that node pulls the image and starts the containers.
6. If you `kubectl delete pod <name>`, actual drops to 1 → step 2 repeats →
   a brand-new Pod (new name, new IP) appears automatically.
   **The Service's IP/DNS doesn't change** — that's the whole point of a Service.

### Job vs Deployment (an honest, *good* interview point)
`mme_sim` is fundamentally a **batch job** — it runs once, produces a pcap,
and is "done". The textbook-correct K8s object for that is a **`Job`**
(runs to completion, doesn't restart on success).

We deliberately used a **Deployment** instead — our entrypoint keeps the
container alive afterwards by serving the pcap over HTTP — *specifically so
we can demo replicas/scaling/self-healing/Services*, which a one-shot Job
can't demonstrate as cleanly. Saying this out loud in an interview shows you
understand the difference, not just that you copy-pasted a YAML file.

---

## 3. Tonight's hands-on commands

### Prereqs (you handle these)
```bash
brew install --cask docker     # Docker Desktop — installs the docker CLI + daemon
open -a Docker                 # launch it, wait for the whale icon to say "running"
```
(`minikube` and `kubectl` are already installed via Homebrew — done.)

### Step 1 — Docker Compose (local, no K8s yet)
```bash
cd mme-simulator
docker compose build
docker compose up
```
In another terminal:
```bash
curl -O http://localhost:8080/mme_capture.pcap   # download the pcap
open mme_capture.pcap                            # opens in Wireshark
```
Stop with Ctrl+C, then `docker compose down`.

### Step 2 — minikube (Kubernetes)
```bash
minikube start                       # creates the local 1-node cluster (takes a few min first time)
minikube image load mme-sim:latest   # copy our image into minikube (no registry needed)

kubectl apply -f k8s/deployment.yaml
kubectl apply -f k8s/service.yaml

kubectl get deployments              # see "mme-sim   2/2"
kubectl get pods                     # see 2 pods, Running
kubectl get svc                      # see mme-sim-svc, NodePort 30080
```

### Step 3 — Reach it
```bash
minikube service mme-sim-svc --url   # prints a URL, open it in browser
```

### Step 4 — Scaling demo
```bash
kubectl scale deployment mme-sim --replicas=4
kubectl get pods -w                  # watch 2 more pods appear
```

### Step 5 — Self-healing demo
```bash
kubectl get pods                     # copy one pod name
kubectl delete pod <pod-name>
kubectl get pods -w                  # watch it terminate AND a new one appear (back to N)
```

### Step 6 — Logs / cleanup
```bash
kubectl logs <pod-name>              # see the mme_sim startup banner + attach flow
kubectl delete -f k8s/               # tear down deployment + service
minikube stop                        # stop the cluster (or `minikube delete` to remove entirely)
```

---

## 4. Interview Q&A

**Q: "What's the difference between Docker and Kubernetes?"**

"Docker packages and runs a container on one machine. Kubernetes orchestrates
many containers across many machines — it handles scaling, self-healing,
and networking between containers, which Docker alone doesn't do. For my
project, Docker builds and runs the `mme-sim` image; Kubernetes (via
minikube) runs 2 replicas of it and keeps them alive."

**Q: "What is a Pod?"**

"The smallest unit Kubernetes schedules — one or more containers that share a
network namespace and storage. In my project, each Pod runs one `mme-sim`
container that executes an LTE attach flow and serves the resulting pcap on
port 8080."

**Q: "What is a Deployment, and how is it different from a ReplicaSet?"**

"A Deployment is a desired-state spec — 'I want 2 replicas of this image'. It
creates and manages a ReplicaSet for you, and additionally handles rollouts —
if I update the image, the Deployment does a rolling update by creating new
ReplicaSets. The ReplicaSet's only job is to keep the Pod count correct; the
Deployment adds versioning/rollout on top."

**Q: "What is a Service, and why do you need one?"**

"Pods are ephemeral — they get new IPs every time they're recreated. A
Service gives you one stable IP/DNS name and load-balances across whatever
Pods currently match its label selector. In my project, `mme-sim-svc` is a
NodePort Service on port 30080 that routes to whichever of the 2 `mme-sim`
Pods is healthy, even after I delete and recreate one."

**Q: "What happens when you delete a Pod that's part of a Deployment?"**

"The ReplicaSet controller notices the actual count dropped below the desired
count and creates a replacement Pod immediately — that's Kubernetes'
self-healing. I demoed this: deleted one of my 2 `mme-sim` pods, and `kubectl
get pods` showed a brand-new pod (different name/IP) appear within seconds,
while the Service kept working throughout."

**Q: "Walk me through the Kubernetes control plane."**

"The API server is the front door — `kubectl` talks to it, and it's the only
thing that talks to etcd directly. etcd stores cluster state — desired and
actual. The scheduler decides which node a new Pod runs on. The controller
manager runs reconciliation loops — e.g. the ReplicaSet controller compares
desired vs actual replica count and creates/deletes Pods to match. On each
node, the kubelet executes those decisions (start/stop containers) and
kube-proxy sets up the networking rules Services rely on."

**Q: "Why a Deployment and not a Job, given mme_sim is a one-shot simulation?"**

"Good catch — technically a Job is the right primitive for a batch task that
runs once to completion. I used a Deployment deliberately: my entrypoint
script keeps the container alive after the simulation finishes by serving the
pcap over HTTP, specifically so I could demonstrate replicas, scaling, and
self-healing, which a Job (by design, doesn't restart on success) can't show
as directly."

**Q: "What's minikube?"**

"A tool that runs a single-node Kubernetes cluster locally, inside a VM/container
on your laptop — for learning and local development without needing real
cloud infrastructure. Everything I did tonight — Deployment, Service, scaling,
self-healing — works identically on a real multi-node cloud cluster."

---

## 5. Optional next step (not tonight): GitHub

This repo is already a local git repo with `gh` (GitHub CLI) available but no
remote configured. Pushing it to a public GitHub repo is straightforward
later (`gh repo create` + `git push`) and rounds out the "wholesome project"
story for LinkedIn — but it's separate from tonight's Docker/K8s work and not
required for the interview.
