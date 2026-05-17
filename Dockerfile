FROM debian:trixie AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential gcc make pkg-config python3 \
    libgtk-4-dev libgtksourceview-5-dev libgtk4-layer-shell-dev \
    dpkg-dev rpm \
    && rm -rf /var/lib/apt/lists/*

ARG TARGETARCH
ARG BUILD=release
ENV TARGETARCH=${TARGETARCH}
ENV BUILD=${BUILD}

COPY . /build
WORKDIR /build

RUN make clean && make BUILD=${BUILD}
RUN mkdir -p /output && python3 scripts/package.py

FROM scratch
COPY --from=builder /output /
