# Build image for GBabe: devkitARM with the 3DS packages, plus bannertool and
# makerom for the .cia. Used by .devcontainer/devcontainer.json and by every
# GitHub Actions job that compiles the 3DS binary, so a build inside the
# devcontainer and a build in CI are the same build.
#
# The toolchain is pinned. Rebuilding the image cannot silently change the
# compiler: devkitARM by digest (that image already carries 3ds-dev: libctru,
# 3dstools, smdhtool, 3dsxtool, bannertool's deps), and the packaging tools by
# tag. To move to a newer devkitARM, take a tag from
# https://hub.docker.com/r/devkitpro/devkitarm/tags together with its index
# digest and change DEVKITARM_IMAGE below - docker/build-image.sh will then hand
# out a different image name, so nothing keeps using the old one by accident.
ARG DEVKITARM_IMAGE=docker.io/devkitpro/devkitarm:20260610@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5

# ---------------------------------------------------------------------------
# Stage 1: build makerom and bannertool.
#
# Neither is in devkitPro's repositories. The published Linux binaries are
# linked against a newer glibc than this image ships (they are built on Ubuntu
# 24.04; devkitpro/devkitarm is Debian bookworm), so running one fails with
# "version `GLIBC_2.39' not found". Building them here costs a few seconds and
# removes that whole class of problem - and they end up in the image, so CI
# never builds them again.
# ---------------------------------------------------------------------------
FROM ${DEVKITARM_IMAGE} AS tools-builder

# Pins live in the script, so local CI runs and the image cannot drift apart.
COPY .github/scripts/install-3ds-packaging-tools.sh /tmp/install-packaging-tools.sh

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        git \
    && rm -rf /var/lib/apt/lists/* \
    && DEST=/usr/local/bin sh /tmp/install-packaging-tools.sh \
    && rm -f /tmp/install-packaging-tools.sh

# ---------------------------------------------------------------------------
# Stage 2: the image everything actually uses.
# ---------------------------------------------------------------------------
FROM ${DEVKITARM_IMAGE}

# Host tools: a native compiler for tools/host and tools/uipreview, Python with
# Pillow for tools/make-artwork.py and tools/make-font.py, and git/gh for
# working inside the container.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        python3 \
        python3-pil \
        git \
        gh \
        openssh-client \
    && rm -rf /var/lib/apt/lists/*

COPY --from=tools-builder /usr/local/bin/bannertool /usr/local/bin/bannertool
COPY --from=tools-builder /usr/local/bin/makerom /usr/local/bin/makerom

ENV DEVKITPRO=/opt/devkitpro \
    DEVKITARM=/opt/devkitpro/devkitARM \
    PATH=/opt/devkitpro/devkitARM/bin:/opt/devkitpro/tools/bin:${PATH}

WORKDIR /workspace
