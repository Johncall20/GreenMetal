#!/bin/bash
# greenmetal-check: read-only look at a machine, to see how close it is to running GreenMetal.
# Changes nothing and needs no root. Run it on the hackintosh itself:   tools/greenmetal-check.sh
# Paste its output into a card report. It prints no serial numbers.
#
# [ok] as expected   [--] information   [!!] needs attention before you try anything
set -u
ok()   { printf '[ok] %s\n' "$*"; }
info() { printf '[--] %s\n' "$*"; }
bad()  { printf '[!!] %s\n' "$*"; PROBLEMS=$((PROBLEMS + 1)); }
PROBLEMS=0

echo "== System"
arch=$(uname -m); ver=$(sw_vers -productVersion 2>/dev/null); build=$(sw_vers -buildVersion 2>/dev/null)
[ "$arch" = x86_64 ] && ok "x86_64" || bad "architecture is $arch; GreenMetal is x86_64 only"
case "$ver" in
  26.*) ok "macOS $ver ($build)";;
  *)    bad "macOS $ver ($build); only macOS 26 has been tried";;
esac

echo "== NVIDIA card"
found=0
while read -r dev class; do
  [ -n "$dev" ] || continue
  case "$class" in 03*) ;; *) continue;; esac          # display controllers only
  found=1; n=$((16#$dev))
  if   [ "$dev" = 1401 ];                     then ok   "10de:$dev  GTX 960, the reference card"
  elif [ $n -ge $((16#1400)) ] && [ $n -le $((16#143f)) ]; then info "10de:$dev  looks like GM206: same chip family as the reference, untested"
  elif [ $n -ge $((16#13c0)) ] && [ $n -le $((16#13ff)) ]; then info "10de:$dev  looks like GM204 (GTX 970/980): closest untested family"
  elif [ $n -ge $((16#17c0)) ] && [ $n -le $((16#17ff)) ]; then info "10de:$dev  looks like GM200 (GTX 980 Ti/Titan X): closest untested family"
  elif [ $n -ge $((16#1340)) ] && [ $n -le $((16#13bf)) ]; then bad  "10de:$dev  looks like first-generation Maxwell (GM107/GM108): needs porting"
  elif [ $n -ge $((16#15f0)) ] && [ $n -le $((16#15ff)) ] || { [ $n -ge $((16#1b00)) ] && [ $n -le $((16#1d8f)) ]; }; then
                                                   bad  "10de:$dev  looks like Pascal: needs porting"
  else                                             bad  "10de:$dev  not a card this project knows about"
  fi
  [ "$dev" = 1401 ] || info "MaxwellFB only matches 10de:1401 today; fb/Info.plist has to be widened for this card"
done < <(ioreg -r -c IOPCIDevice -d 1 -l -w0 2>/dev/null | awk '
  function flush() { if (ven == "de10") print dev, cls; ven = dev = cls = "" }
  /\+-o /            { flush() }
  /"vendor-id" = </  { v = $0; sub(/.*</, "", v); ven = substr(v, 1, 4) }
  /"device-id" = </  { v = $0; sub(/.*</, "", v); dev = substr(v, 3, 2) substr(v, 1, 2) }
  /"class-code" = </ { v = $0; sub(/.*</, "", v); cls = substr(v, 5, 2) substr(v, 3, 2) }
  END                { flush() }')
[ $found = 1 ] || bad "no NVIDIA display device found"

echo "== Kexts loaded"
loaded=$(kmutil showloaded --list-only 2>/dev/null | awk '{print $6, $7}')
has() { printf '%s\n' "$loaded" | grep -m1 "^$1 " ; }
for k in com.nvidia.web.NVDAResmanWeb com.nvidia.NVDAStartupWeb; do
  l=$(has "$k") && ok "$l" || bad "$k is not loaded: the Web Driver resource manager is required"
done
hal=$(printf '%s\n' "$loaded" | grep -m1 '^com\.nvidia\.web\.NVDA.*Hal')
[ -n "$hal" ] && ok "$hal" || bad "no Web Driver HAL kext loaded"
case "$hal" in *GM100*|"") ;; *) info "this HAL is not the Maxwell one the project was written against";; esac
printf '%s\n' "$loaded" | grep -q '^com\.nvidia\.web\.NVDAResmanWeb (10\.3\.3)' || info "the reference Web Driver is 10.3.3 (387.10.10.10.40.140); layouts may differ on another version"
for k in as.vit9696.Lilu as.vit9696.WhateverGreen com.dhinakg.AMFIPass; do
  l=$(has "$k") && ok "$l" || info "$k not loaded"
done
l=$(has com.nvidia.web.GeForceWeb) && info "$l  (the NVIDIA accelerator; MaxwellAccel replaces it in the full setup)"
gm=$(printf '%s\n' "$loaded" | grep '^local\.maxwell\.')
if [ -n "$gm" ]; then printf '%s\n' "$gm" | while read -r l; do ok "$l"; done; else info "no GreenMetal kext loaded"; fi

echo "== SIP and boot-args"
sip=$(csrutil status 2>/dev/null)
echo "$sip" | grep -q 'Kext Signing: disabled' && ok "SIP: kext signing disabled" || bad "SIP: kext signing is enforced; unsigned kexts will not load"
echo "$sip" | grep -q 'Filesystem Protections: disabled' && ok "SIP: filesystem protections disabled" || info "SIP: filesystem protections enabled"
args=$(nvram boot-args 2>/dev/null | cut -f2-)
info "boot-args: ${args:-(none)}"
case " $args " in *" ngfxgl=1 "*) ok "ngfxgl=1 present (keeps NVIDIA's 2018 Metal plugin out of the way)";;
                  *) bad "ngfxgl=1 missing: without it WindowServer may crash-loop on NVIDIA's old Metal plugin";; esac
case " $args " in *" keepsyms=1 "*) ok "keepsyms=1 present (readable panics)";; *) info "add keepsyms=1 debug=0x100 for readable panic reports";; esac
case " $args " in *" amfi="*|*" amfi_get_out_of_my_way="*) ok "AMFI relaxed by boot-arg";; *) info "no amfi boot-arg; the unsigned Metal plugin may be refused";; esac

echo "== GreenMetal user-space stack"
GB=/Library/GPUBundles
if [ -d "$GB/NVMTLDriver.bundle" ]; then
  ok "$GB/NVMTLDriver.bundle"
  for f in nvmtl/libvulkan_nouveau.dylib nvmtl/libnvmtl_translate.dylib nvmtl-allow.txt; do
    [ -e "$GB/$f" ] && ok "$GB/$f" || bad "$GB/$f missing"
  done
else
  info "nothing installed in $GB"
fi
n=$(ioreg -r -c NVAccel -d 1 2>/dev/null | grep -c '+-o')
[ "$n" -gt 0 ] && ok "accelerator service present" || info "no NVAccel service"
a=$(sysctl -n debug.nvaccelfb 2>/dev/null) && info "debug.nvaccelfb = $a (1 means the accelerator is armed)"

echo "== A way back"
nc -z -w1 127.0.0.1 22 >/dev/null 2>&1 && ok "ssh is listening (you can recover with a black screen)" \
  || bad "ssh is off: turn on Remote Login before you install any kext"
n=$(diskutil apfs listSnapshots / 2>/dev/null | grep -c 'com.apple.os.update-')
[ "$n" -gt 0 ] && ok "sealed system snapshot present" || info "no com.apple.os.update-* snapshot found"
[ -d /Applications/OpenCore-Patcher.app ] && info "OpenCore Legacy Patcher is installed (the full setup removes its non-Metal patches)"
info "not checked: a bootable EFI backup and a system clone. Have both."

echo
[ $PROBLEMS -eq 0 ] && echo "No blockers found." || echo "$PROBLEMS item(s) need attention."
exit 0
