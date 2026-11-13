#!/bin/bash
# Testa variantes do flip do canal base (debug.mxflip da MaxwellMetal 0.7.1) sem reiniciar. Rodar com sudo.
# Cada variante: flip magenta por 4 s e volta para a home. Para na primeira em que o flip ARMA (debug.mxflip_iso != 0).
# Bits: 1 intervalo 1 | 2 zera notifier/semaforo | 4 CSC base | 8 LUT = USE_CORE_LUT | 0x40 storage do core | 0x80 ctxdma sem KIND_PITCH
[ "$(id -u)" -eq 0 ] || { echo "precisa de root (sudo)"; exit 1; }
T=$(cd "$(dirname "$0")" && pwd)
sysctl debug.mxflip >/dev/null 2>&1 || { echo "PARADA: MaxwellMetal sem debug.mxflip (precisa da 0.7.1)"; exit 1; }
tenta() {  # $1 = mxflip, $2 = storage forcado (0 = nao), $3 = descricao
  sysctl -w debug.mxflip=$1 debug.mxflip_storage=$2 >/dev/null
  echo "== mxflip $1 storage $2: $3  (olhe a tela: MAGENTA por 4 s?)"
  "$T/abi_test" 0xFFFF00FF 4 >/dev/null 2>&1
  iso=$(sysctl -n debug.mxflip_iso); exc=$(sysctl -n debug.mxflip_exc); cod=$(sysctl -n debug.mxflip_code)
  printf "   ISO em uso 0x%x  excecao 0x%x  codigo 0x%x  -> %s\n" "$iso" "$exc" "$cod" "$([ "$iso" != 0 ] && echo ARMOU || echo "nao armou")"
  [ "$iso" != 0 ]
}
tenta 79  0 "sequencia completa + storage do core + LUT do core" && { echo "FUNCIONOU com mxflip 79 (0x4f). Fica assim ate o proximo boot."; exit 0; }
tenta 15  0 "sequencia completa + pitch + LUT do core"          && { echo "FUNCIONOU com mxflip 15 (0x0f). Fica assim ate o proximo boot."; exit 0; }
tenta 207 0 "0x4f + ctxdma sem KIND_PITCH"                      && { echo "FUNCIONOU com mxflip 207 (0xcf). Fica assim ate o proximo boot."; exit 0; }
tenta 72  0 "so storage do core + LUT do core (flip curto)"     && { echo "FUNCIONOU com mxflip 72 (0x48). Fica assim ate o proximo boot."; exit 0; }
tenta 8   0 "so LUT do core (flip curto, pitch)"                && { echo "FUNCIONOU com mxflip 8. Fica assim ate o proximo boot."; exit 0; }
tenta 107 0 "storage e LUT do core + CSC do core em vez da base"  && { echo "FUNCIONOU com mxflip 107 (0x6b)."; exit 0; }
echo "nenhuma variante armou. Log do backend:"
/usr/bin/log show --last 3m --style compact --predicate 'sender == "MaxwellMetal"' 2>/dev/null | grep "flip " | tail -14 | cut -c60-330
