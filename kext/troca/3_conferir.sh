#!/bin/bash
# Depois da troca (sem root; tambem por ssh): o que subiu e onde parou.
echo "== boot-args"; nvram boot-args 2>/dev/null | cut -f2-
echo "== kexts"; kextstat 2>/dev/null | grep -E "NVDA|GeForce|Maxwell|IOAcceleratorFamily2|IOSurface " | awk '{print "   " $6, $7}'
echo "== servicos"
for c in NVDA MaxwellMetal NVAccel; do n=$(ioreg -r -c "$c" -d 1 2>/dev/null | grep -c "+-o"); echo "   $c: $n"; done
ioreg -r -c NVAccel -d 1 -l 2>/dev/null | grep -E "MetalPluginName|NVAccelEnabledFrom|IOGLBundleName" | sed 's/^ */   /'
echo "== sysctl"; sysctl debug.nvaccelfb debug.nvaccel_headless 2>/dev/null | sed 's/^/   /'
echo "== WindowServer"; pgrep -lx WindowServer | sed 's/^/   /' || echo "   NAO esta rodando"
echo "== log do acelerador e do backend (5 min)"
/usr/bin/log show --last 5m --style compact --predicate 'sender == "MaxwellAccel" OR sender == "MaxwellMetal"' 2>/dev/null \
  | grep -v "ponte:" | tail -40 | cut -c1-220
echo "== plugin Metal"; tail -20 /tmp/nvmtl.log 2>/dev/null | cut -c1-200
