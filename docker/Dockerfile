# The srthub binary is built on the host and copied in below, so this base image
# must match the host's Ubuntu release - a container with an older glibc than the
# build host will fail to load the binary. install.sh passes the host's version
# automatically; the default here is only a fallback for manual builds.
ARG UBUNTU_VERSION=24.04
FROM ubuntu:${UBUNTU_VERSION}

LABEL maintainer="cannonbeachgoonie@gmail.com"
LABEL org.opencontainers.image.source="https://github.com/cannonbeach/opensrthub"

ENV DEBIAN_FRONTEND=noninteractive

# Single layer instead of nine, with the apt lists cleaned up afterwards.
RUN apt-get update \
 && apt-get -y upgrade \
 && apt-get install -y --no-install-recommends \
        apt-utils \
        libssl-dev \
        libcrypt-dev \
        libnuma1 \
        net-tools \
        iputils-ping \
        tcpdump \
 && apt-get clean \
 && rm -rf /var/lib/apt/lists/*

COPY srthub /usr/bin/srthub
RUN chmod 0755 /usr/bin/srthub
