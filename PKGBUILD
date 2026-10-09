pkgname=metroid-prime-port-bin
pkgver=0.19.0
pkgrel=1
pkgdesc="Native Linux port of Metroid Prime"
arch=('x86_64')
url='https://github.com/Odrannnn/MetroidPrimePort'
license=('custom:MIT')
source=(
    "https://github.com/Odrannnn/MetroidPrimePort/releases/download/v${pkgver}/metroid-prime-port-${pkgver}-linux-x86_64.tar.gz"
    "https://raw.githubusercontent.com/Odrannnn/MetroidPrimePort/v${pkgver}/packaging/io.github.odrannnn.metroidprimeport.desktop"
    "https://raw.githubusercontent.com/Odrannnn/MetroidPrimePort/v${pkgver}/packaging/metroid_prime_port.png"
)
sha256sums=('ce23e05d71331cc7cd76789810797ed40b828b96fc248a049e99413f750dbd2d'
            '6ad8e78e4dfc27cd5aec8082de65166fbf8cfbd6e9580dfd7eb47ac514f7cd51'
            '3350d8fb6a19d38b5ef6c9835829ee5f12783c8030563d5fda7d6dca0e7313a8')

package() {
    install -Dm755 \
        "metroid-prime-port-linux-x86_64/metroid_prime_port" \
        "$pkgdir/usr/bin/metroid_prime_port"

    install -Dm644 \
        "metroid-prime-port-linux-x86_64/share/licenses/metroid-prime-port/LICENSE" \
        "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    install -Dm644 \
        "metroid-prime-port-linux-x86_64/share/licenses/metroid-prime-port/NOTICE" \
        "$pkgdir/usr/share/licenses/$pkgname/NOTICE"
    install -Dm644 \
        "$srcdir/io.github.odrannnn.metroidprimeport.desktop" \
        "$pkgdir/usr/share/applications/io.github.odrannnn.metroidprimeport.desktop"

    install -Dm644 \
        "$srcdir/metroid_prime_port.png" \
        "$pkgdir/usr/share/icons/hicolor/256x256/apps/io.github.odrannnn.metroidprimeport.png"
}


