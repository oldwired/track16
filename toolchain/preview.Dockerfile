# The toolchain image plus what the build machine itself needs for scripts/preview.sh (ALSA headers for the host).
ARG BASE=sp16-toolchain:gcc11-egl
FROM ${BASE}
USER root
RUN apt-get update && apt-get install -y --no-install-recommends libasound2-dev && rm -rf /var/lib/apt/lists/*
