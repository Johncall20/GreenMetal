#!/bin/bash
# Stages dist/GreenMetal-<version>/ from the binaries already built in this tree and packs it.
# It builds nothing: build the kexts, NVK, the plugin and the translator first (docs/INSTALL.md).
#
# What goes in: the committed scripts and documents, our four kexts, the Metal plugin bundle, NVK and the shader
# translator. What never goes in: anything from NVIDIA or Apple. The script checks that, and stops if a binary is
# not x86_64, is not signed, or carries a home-directory path.
set -euo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
V=$(cat "$R/VERSION"); N=GreenMetal-$V; O=$R/dist; S=$O/$N
die() { echo "STOP: $*" >&2; exit 1; }
cd "$R"

TEXT="greenmetal VERSION LICENSE NOTICE.md LICENSES README.md README.pt-BR.md docs/INSTALL.md docs/INSTALL.pt-BR.md
      tools/greenmetal-check.sh kext/deploy kext/troca kext/gpubundles"
# shellcheck disable=SC2086
git diff --quiet HEAD -- $TEXT || die "uncommitted changes in files that go into the release; commit first"

rm -rf "$S"; mkdir -p "$S"
# shellcheck disable=SC2086
git ls-files -z -- $TEXT | COPYFILE_DISABLE=1 tar cf - --null -T - | tar xf - -C "$S"

echo "== binaries"
put() { [ -e "$1" ] || die "not built: $1"; mkdir -p "$(dirname "$S/$2")"; ditto --norsrc --noextattr "$1" "$S/$2"; echo "   $2"; }
put kext/MaxwellMetal/build/MaxwellMetal.kext kext/deploy/MaxwellMetal.kext
put accel/build/MaxwellAccel.kext             kext/troca/MaxwellAccel.kext
put agdc/build/MaxwellAGDC.kext               kext/troca/MaxwellAGDC.kext
put fb/build/MaxwellFB.kext                   kext/troca/MaxwellFB.kext
put kext/deploy/mtlprobe                      kext/deploy/mtlprobe
put kext/gpubundles/NVMTLDriver.bundle        kext/gpubundles/NVMTLDriver.bundle
put kext/gpubundles/nvmtl/libvulkan_nouveau.dylib   kext/gpubundles/nvmtl/libvulkan_nouveau.dylib
put kext/gpubundles/nvmtl/libnvmtl_translate.dylib  kext/gpubundles/nvmtl/libnvmtl_translate.dylib

echo "== third-party licences"
T=$S/LICENSES/third-party; mkdir -p "$T"
lic() { [ -f "$1" ] || die "licence text not found: $1"; cp "$1" "$T/$2"; echo "   $2"; }
lic nvk/mesa/docs/license.rst                         Mesa-license.rst
lic metal/llvm/llvm-project-21.1.8.src/llvm/LICENSE.TXT  LLVM-LICENSE.txt
lic metal/translator-src/LICENSE                      translator-LGPL-3.0.txt
lic metal/translator-src/COPYING                      translator-GPL-3.0.txt
lic ref/nullmoth/NOTICE                               NullMoth-NOTICE.txt
cat > "$S/LICENSES/THIRD-PARTY-BINARIES.md" <<'EOF'
# Binaries in this archive and their licences

| File | What it is | Licence |
|---|---|---|
| `kext/deploy/MaxwellMetal.kext`, `kext/troca/MaxwellFB.kext`, `kext/deploy/mtlprobe` | GreenMetal | PolyForm Noncommercial 1.0.0 |
| `kext/troca/MaxwellAccel.kext`, `kext/troca/MaxwellAGDC.kext` | GreenMetal, derived from NullMoth's NVAccel and NVRMAGDC (v2.7.2) | PolyForm Noncommercial 1.0.0 |
| `kext/gpubundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver` | NullMoth's Metal plugin (v2.7.2) with GreenMetal's changes | PolyForm Noncommercial 1.0.0 |
| `kext/gpubundles/NVMTLDriver.bundle/Contents/MacOS/air-runtime/libLLVM.dylib` | LLVM 21.1.8, unmodified | Apache-2.0 WITH LLVM-exception (`third-party/LLVM-LICENSE.txt`) |
| `kext/gpubundles/nvmtl/libvulkan_nouveau.dylib` | Mesa NVK at `17ca6174` with NullMoth's macOS patch and GreenMetal's patch, plus libnvrm387 | MIT (`third-party/Mesa-license.rst`, `MIT.txt`) |
| `kext/gpubundles/nvmtl/libnvmtl_translate.dylib` | NullMoth's shader translator (v2.7.2), based on metal2vulkan by Anees Iqbal, unmodified source | LGPL-3.0-or-later (`third-party/translator-LGPL-3.0.txt`, `third-party/translator-GPL-3.0.txt`) |

Source for everything GreenMetal wrote or changed is in the repository this archive was released from, at the tag
of the same version. The translator's complete source is published next to this archive as
`nvmtl-translator-source.tar.gz`, and upstream at <https://github.com/nullmoth/nvidia-macos-driver> (tag v2.7.2,
directory `translator/`). The only change made to the translator binary after it was built is that home-directory
paths recorded by the compiler were overwritten with `x` characters.

No file in this archive comes from NVIDIA's Web Driver or from Apple. The Web Driver is required at run time and
you supply it yourself.
EOF

# Raw byte count: strings(1) does not look inside the symbol string table, where the debug map keeps object paths.
homes() { perl -0777 -ne 'my $c = () = m{/Users/[^x/]}g; print $c' "$1"; }

echo "== scrub home-directory paths recorded by the compiler"
find "$S" -type f | while IFS= read -r f; do
  file -b "$f" | grep -q 'Mach-O' || continue
  n=$(homes "$f")
  [ "$n" -gt 0 ] || continue
  case "$f" in *.kext/*) die "a kext carries a /Users/ path ($f); rebuild it from a neutral path instead of patching";; esac
  perl -pi -e 's{/Users/([A-Za-z0-9._-]+)/}{"/Users/" . ("x" x length($1)) . "/"}ge' "$f"
  codesign -f -s - "$f" >/dev/null 2>&1 || die "re-sign failed: $f"
  echo "   ${f#"$S"/}: $n paths overwritten, re-signed"
done

echo "== checks"
bad=0
while IFS= read -r f; do
  file -b "$f" | grep -q 'Mach-O' || continue
  a=$(lipo -archs "$f" 2>/dev/null)
  [ "$a" = x86_64 ] || { echo "   not x86_64 ($a): $f"; bad=1; }
  codesign -v "$f" 2>/dev/null || { echo "   bad signature: $f"; bad=1; }
  h=$(homes "$f")
  [ "$h" -eq 0 ] || { echo "   home-directory path left: $f"; bad=1; }
done < <(find "$S" -type f)
codesign -v "$S/kext/gpubundles/NVMTLDriver.bundle" 2>/dev/null || { echo "   bad bundle signature"; bad=1; }
while IFS= read -r p; do
  id=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$p" 2>/dev/null || true)
  case "$id" in com.nvidia.*|com.apple.*) echo "   foreign bundle: $id ($p)"; bad=1;; esac
done < <(find "$S" -name Info.plist)
find "$S" \( -name '.DS_Store' -o -name '._*' \) -delete
[ $bad -eq 0 ] || die "checks failed"
echo "   all Mach-O files are x86_64, signed, and free of home-directory paths; no NVIDIA or Apple bundle"

echo "== pack"
OWNER="--uid 0 --gid 0 --uname root --gname wheel"   # do not record the build user in the archive
( cd "$O" && COPYFILE_DISABLE=1 tar czf "$N.tar.gz" $OWNER "$N" )
if [ -d metal/translator-src ]; then
  COPYFILE_DISABLE=1 tar czf "$O/nvmtl-translator-source.tar.gz" $OWNER --exclude target --exclude .DS_Store -C metal translator-src
fi
( cd "$O" && shasum -a 256 "$N.tar.gz" nvmtl-translator-source.tar.gz 2>/dev/null > SHA256SUMS.txt && cat SHA256SUMS.txt )
ls -la "$O"/*.tar.gz
echo "ok: $O/$N.tar.gz"
