# SPDX-License-Identifier: BSD-3-Clause
#
# Builds and tests vqda and exports the aarch64 install tree:
#   docker build --output out .
FROM debian:trixie-slim AS build

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates gcc libc6-dev linux-libc-dev git meson ninja-build \
        $([ "$(dpkg --print-architecture)" = arm64 ] || \
          echo gcc-aarch64-linux-gnu libc6-dev-arm64-cross) && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .
RUN git config --global --add safe.directory /src && scripts/ci.sh /out

FROM scratch
COPY --from=build /out /
