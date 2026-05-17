#!/bin/bash
set -euo pipefail

VERSION=$(cat /build/VERSION)
ARCH="${TARGETARCH}"
OUT="/output"

# ── arch mapping ─────────────────────────────────────────────
case "$ARCH" in
    amd64)  DEB_ARCH=amd64  ;;
    arm64)  DEB_ARCH=arm64  ;;
    *)      echo "Unknown arch: $ARCH"; exit 1 ;;
esac
case "$ARCH" in
    amd64)  RPM_ARCH=x86_64 ;;
    arm64)  RPM_ARCH=aarch64 ;;
    *)      RPM_ARCH="$ARCH" ;;
esac

mkdir -p "$OUT"

# ── .deb ─────────────────────────────────────────────────────
DEB_NAME="promptr_${VERSION}_${DEB_ARCH}"

mkdir -p "/pkg-deb/DEBIAN" "/pkg-deb/usr/bin" \
    "/pkg-deb/usr/share/icons/hicolor/scalable/apps" \
    "/pkg-deb/usr/share/applications"
cp /build/promptr "/pkg-deb/usr/bin/promptr"
cp /build/data/promptr.svg "/pkg-deb/usr/share/icons/hicolor/scalable/apps/promptr.svg"
cp /build/com.toxdes.promptr.desktop "/pkg-deb/usr/share/applications/com.toxdes.promptr.desktop"
chmod 755 "/pkg-deb/usr/bin/promptr"

cat > "/pkg-deb/DEBIAN/control" <<EOF
Package: promptr
Version: ${VERSION}
Architecture: ${DEB_ARCH}
Maintainer: toxdes <toxdes@proton.me>
Section: utils
Priority: optional
Depends: libgtk-4-1, libgtksourceview-5-0, libgtk4-layer-shell0
Description: GTK4 overlay prompt for opencode
 promptr is a GTK4 overlay application for running opencode
 commands through a resizable, always-on-top window.
EOF

dpkg-deb --build "/pkg-deb" "$OUT/${DEB_NAME}.deb"
rm -rf /pkg-deb

echo "  -> $OUT/${DEB_NAME}.deb"

# ── .rpm ─────────────────────────────────────────────────────
RPM_NAME="promptr-${VERSION}-1.${RPM_ARCH}"

mkdir -p /tmp/rpm/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

cat > "/tmp/rpm/SPECS/promptr.spec" <<EOF
Name:           promptr
Version:        ${VERSION}
Release:        1%{?dist}
Summary:        GTK4 overlay prompt for opencode
License:        MIT
BuildArch:      ${RPM_ARCH}
Requires:       gtk4
Requires:       gtksourceview5
Requires:       gtk4-layer-shell

%description
promptr is a GTK4 overlay application for running opencode
commands through a resizable, always-on-top window.

%install
mkdir -p %{buildroot}/usr/bin \
         %{buildroot}/usr/share/icons/hicolor/scalable/apps \
         %{buildroot}/usr/share/applications
install -m755 /build/promptr %{buildroot}/usr/bin/promptr
install -m644 /build/data/promptr.svg %{buildroot}/usr/share/icons/hicolor/scalable/apps/promptr.svg
install -m644 /build/com.toxdes.promptr.desktop %{buildroot}/usr/share/applications/com.toxdes.promptr.desktop

%files
/usr/bin/promptr
/usr/share/icons/hicolor/scalable/apps/promptr.svg
/usr/share/applications/com.toxdes.promptr.desktop
EOF

rpmbuild -bb --define "_topdir /tmp/rpm" \
    "/tmp/rpm/SPECS/promptr.spec"

cp "/tmp/rpm/RPMS/${RPM_ARCH}/${RPM_NAME}.rpm" "$OUT/"
rm -rf /tmp/rpm

echo "  -> $OUT/${RPM_NAME}.rpm"
echo "Done."
