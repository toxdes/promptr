FROM debian:bookworm

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential gcc make pkg-config \
    libgtk-4-dev libgtksourceview-5-dev libgtk4-layer-shell-dev \
    dpkg-dev rpm \
    && rm -rf /var/lib/apt/lists/*

ARG TARGETARCH
ENV TARGETARCH=${TARGETARCH}

COPY . /build
WORKDIR /build

RUN make
RUN bash scripts/package.sh
