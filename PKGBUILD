# Maintainer: xander-lin

pkgname=fcitx5-vinput-git
_pkgname=fcitx5-vinput
pkgver=0.1.0.r125.d40259f
pkgrel=1
pkgdesc="Voice input addon for fcitx5: push-to-talk ASR via CapsLock"
arch=('x86_64')
url="https://github.com/xander-lin/vinput"
license=('MIT')
depends=('fcitx5' 'libebur128' 'libpulse' 'curl' 'speexdsp' 'libsoxr')
makedepends=('meson' 'ninja')
provides=("$_pkgname")
conflicts=("$_pkgname")
install=PKGBUILD.install
source=()
sha256sums=()

pkgver() {
    cd "$startdir"
    printf "0.1.0.r%s.%s" "$(git rev-list --count HEAD 2>/dev/null || printf 0)" "$(git rev-parse --short HEAD 2>/dev/null || printf local)"
}

prepare() {
    rm -rf "$srcdir/$_pkgname"
    mkdir -p "$srcdir/$_pkgname"
    cp -a \
        "$startdir/ASR_provider" \
        "$startdir/adapter" \
        "$startdir/config" \
        "$startdir/docs" \
        "$startdir/tests" \
        "$startdir/tools" \
        "$startdir/meson.build" \
        "$startdir/README.md" \
        "$startdir/LICENSE" \
        "$srcdir/$_pkgname/"
}

build() {
    cd "$_pkgname"
    meson setup build --prefix=/usr --buildtype=plain
    meson compile -C build
}

package() {
    cd "$_pkgname"
    DESTDIR="$pkgdir" meson install -C build
    install -Dm644 README.md "$pkgdir/usr/share/doc/$_pkgname/README.md"
    install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$_pkgname/LICENSE"
    for f in config/*.json.example; do
        install -Dm644 "$f" "$pkgdir/usr/share/doc/$_pkgname/$f"
    done
}
