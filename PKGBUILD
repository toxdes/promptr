# Maintainer: toxdes <toxdes@proton.me>
pkgname=promptr-git
pkgrel=1
pkgdesc="GTK4 overlay prompt for opencode"
arch=('x86_64' 'aarch64')
url="https://github.com/toxdes/promptr"
license=('MIT')
depends=('gtk4' 'gtksourceview5' 'gtk4-layer-shell')
makedepends=('git' 'gcc' 'make' 'pkg-config')
source=("git+${url}.git")
sha256sums=('SKIP')

pkgver() {
  cd "$srcdir/promptr"
  git describe --long --tags | sed 's/^v//;s/-/./g'
}

build() {
  cd "$srcdir/promptr"
  make BUILD=release
}

package() {
  cd "$srcdir/promptr"
  make install PREFIX=/usr DESTDIR="$pkgdir"
}
