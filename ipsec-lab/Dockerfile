# Alpine Linux = tiny (~5MB). strongSwan = the IPSec daemon.
# tcpdump = packet capture. iproute2 = ip xfrm commands.
FROM alpine:3.19

RUN apk add --no-cache \
    strongswan \
    tcpdump \
    iproute2 \
    iputils \
    bash \
    openssl

# strongSwan config goes here
WORKDIR /etc/swanctl
