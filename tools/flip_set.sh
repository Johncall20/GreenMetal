#!/bin/bash
# Aplica uma variante do flip (debug.mxflip / debug.mxflip_storage da MaxwellMetal 0.7.1) e refaz o flip da home.
# Uso: sudo flip_set.sh <mxflip> <storage>      ex.: sudo flip_set.sh 79 7684
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 1; }
[ $# -eq 2 ] || { echo "uso: $0 <mxflip> <storage>"; exit 1; }
T=$(cd "$(dirname "$0")" && pwd)
sysctl -w debug.mxflip=$1 debug.mxflip_storage=$2 >/dev/null
"$T/abi_test" 0xFFFF00FF 1 >/dev/null 2>&1
iso=$(sysctl -n debug.mxflip_iso); cod=$(sysctl -n debug.mxflip_code)
printf "mxflip %s storage 0x%x -> ISO em uso 0x%x (%s)\n" "$1" "$2" "$((iso & 0xffffffff))" "$([ "$iso" != 0 ] && echo ARMOU || echo "nao armou, codigo $cod")"
/usr/bin/log show --last 20s --style compact --predicate 'sender == "MaxwellMetal"' 2>/dev/null | grep "flip home" | tail -1 | cut -c60-300
