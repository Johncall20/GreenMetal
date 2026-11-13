#!/bin/bash
# Uma vez por boot (LaunchDaemon local.maxwell.mxfb-ws): o RM r387 so fica pronto depois que o WindowServer abre o
# NVDA, entao o MaxwellFB aparece depois do WindowServer, que nao procura framebuffer novo. Quando o MaxwellFB aparece
# e o WindowServer ainda nao o abriu, reinicia o WindowServer uma vez.
LOG=/var/log/mxfb_ws.log
log() { echo "$(date '+%F %T') $*" >> "$LOG"; }
has_fb() { /usr/sbin/ioreg -r -c MaxwellFB -d 1 2>/dev/null | grep -q '+-o MaxwellFB'; }
fb_open() { /usr/sbin/ioreg -r -c MaxwellFB -d 2 2>/dev/null | grep -q IOFramebufferUserClient; }
log "inicio"
for i in $(seq 1 300); do has_fb && break; sleep 1; done
has_fb || { log "sem MaxwellFB em 300 s: nada a fazer"; exit 0; }
log "MaxwellFB presente (${i} s)"
sleep 3
if fb_open; then log "o WindowServer ja abriu o MaxwellFB"; exit 0; fi
if ! pgrep -x WindowServer >/dev/null; then log "WindowServer nao esta rodando: ele acha o MaxwellFB sozinho"; exit 0; fi
log "reiniciando o WindowServer para ele abrir o MaxwellFB"
/bin/launchctl kickstart -k system/com.apple.WindowServer
for i in $(seq 1 30); do fb_open && { log "o WindowServer abriu o MaxwellFB (${i} s)"; exit 0; }; sleep 1; done
log "o WindowServer voltou mas nao abriu o MaxwellFB em 30 s"
