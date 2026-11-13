#!/bin/bash
# Estado do MaxwellFB depois do boot (sem root; tambem por ssh).
echo "== kexts"; kmutil showloaded 2>/dev/null | grep -E "Maxwell" | awk '{print "   " $6, $7}'
echo "== MaxwellFB no IORegistry"
ioreg -r -c MaxwellFB -w0 -l 2>/dev/null | grep -E "\+-o|IOFBDependent|MaxwellFBPhys|IOFBCurrentPixelClock|\"IOFBConfig\"" | cut -c1-160 | sed 's/^/   /'
ioreg -r -c MaxwellFB -w0 -d 3 2>/dev/null | grep -E "\+-o" | sed 's/^/   /'
echo "== sysctl"; sysctl debug.nvaccelfb debug.nvaccel_iop debug.nvrmfb_agdc 2>/dev/null | sed 's/^/   /'
echo "== WindowServer"; pgrep -lx WindowServer | sed 's/^/   /' || echo "   NAO esta rodando"
echo "== log do kernel (MaxwellFB/AGDC/acelerador, 10 min)"
/usr/bin/log show --last 10m --style compact --predicate 'sender == "MaxwellFB" OR sender == "MaxwellAGDC" OR sender == "MaxwellAccel"' 2>/dev/null \
  | grep -v "ponte:" | grep -E "MaxwellFB|AGDC|framebuffer|published|pipe" | tail -50 | cut -c1-220
echo "== WindowServer/IOPresentment (10 min)"
/usr/bin/log show --last 10m --style compact --predicate 'process == "WindowServer"' 2>/dev/null \
  | grep -iE "capabilit|InstallTiming|preflight|compositor|display.*online|MaxwellFB|0x2006" | tail -25 | cut -c1-220
echo "== daemon mxfb-ws"; tail -8 /var/log/mxfb_ws.log 2>/dev/null | sed "s/^/   /" || echo "   sem log"
