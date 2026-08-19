FROM debian:trixie AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential gcc make pkg-config python3 curl ca-certificates \
    libgtk-4-dev libgtksourceview-5-dev libgtk4-layer-shell-dev \
    libcurl4-openssl-dev libjson-glib-dev \
    dpkg-dev rpm \
    && update-ca-certificates \
    && rm -rf /var/lib/apt/lists/*

ARG TARGETARCH
ARG VERSION
ARG GIT_SHA=unknown
ARG BUILD=release
ARG INCLUDE_APPIMAGE=0
ENV TARGETARCH=${TARGETARCH}
ENV BUILD=${BUILD}
ENV INCLUDE_APPIMAGE=${INCLUDE_APPIMAGE}

RUN if [ "$INCLUDE_APPIMAGE" = "1" ]; then \
      apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates squashfs-tools && \
      update-ca-certificates && \
      rm -rf /var/lib/apt/lists/* && \
      curl -Lo /usr/local/share/appimage-runtime \
        "https://github.com/AppImage/AppImageKit/releases/download/continuous/runtime-$(uname -m)" && \
      chmod +x /usr/local/share/appimage-runtime; \
    fi

COPY . /build
WORKDIR /build

RUN make clean && make BUILD=${BUILD}
RUN mkdir -p /output && python3 yesb/package.py

FROM scratch
COPY --from=builder /output /
