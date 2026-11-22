#!/bin/sh
set -eu

# Install the pinned native Mermaid renderer for this user or a supplied prefix.
prefix=${1:-"$HOME/.local"}
version=0.11.0
case "$(uname -s)/$(uname -m)" in
    Linux/x86_64)
        target=x86_64-unknown-linux-gnu
        digest=cb51eae6818fb1e6f14ef469ea0b2a67b560c345bf3065513d24f640fa3334b2 ;;
    Linux/aarch64)
        target=aarch64-unknown-linux-gnu
        digest=5bdbdedd91e8aafd4127ea996bccb5d25052e8a49098d8fca903e00f1d60781f ;;
    Darwin/arm64)
        target=aarch64-apple-darwin
        digest=fb22df8fd8f39431bd217d80d42ad73c5e95190609ecd0a80604b11b195f1701 ;;
    Darwin/x86_64)
        target=x86_64-apple-darwin
        digest=727519471b409266d76d24fb985b57e31bedebece3ca2185615c212f999d5bcd ;;
    *)
        echo "No prebuilt Merman CLI for $(uname -s)/$(uname -m)." >&2
        exit 1 ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
package=merman-cli-$target
url=https://github.com/Latias94/merman/releases/download/v$version/$package.tar.xz
curl --fail --location --silent --show-error --retry 3 "$url" -o "$tmp/package.tar.xz"
if command -v sha256sum >/dev/null 2>&1; then
    checksum=sha256sum
else
    checksum="shasum -a 256"
fi
(cd "$tmp" && printf '%s  package.tar.xz\n' "$digest" | $checksum -c -)
tar -xJf "$tmp/package.tar.xz" -C "$tmp"
licenses="$prefix/share/licenses/slate/merman"
mkdir -p "$prefix/bin" "$licenses"
cp -R "$tmp/$package/THIRD_PARTY_LICENSES" "$licenses/"
cp "$tmp/$package/LICENSE-APACHE" "$tmp/$package/LICENSE-MIT" "$tmp/$package/THIRD_PARTY_NOTICES.md" "$licenses/"
install -m 755 "$tmp/$package/merman-cli" "$prefix/bin/merman-cli"
echo "Installed Merman CLI $version in $prefix/bin."
