#!/usr/bin/env bash
# ============================================================================
# Genera firmware/src/red/certificados.h a partir del servidor REAL.
#
#   ./tools/scripts/generar-ca.sh https://TU_PROYECTO.supabase.co
#
# POR QUÉ NO SE PUEDE FIJAR UN CERTIFICADO Y OLVIDARSE:
# Las autoridades rotan sus raíces, y Let's Encrypt lo hace con cierta
# frecuencia. El día que la raíz embebida deje de firmar el certificado del
# servidor, el equipo dejará de publicar con un error de TLS, y desde planta
# parecerá que "se cayó internet". Este script permite regenerar el encabezado
# y cargarlo por OTA antes de que eso ocurra.
#
# El encabezado incluye la fecha de expiración de cada raíz. Conviene revisarlas
# en el mantenimiento trimestral.
# ============================================================================
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SALIDA="$RAIZ/firmware/src/red/certificados.h"
URL="${1:-https://supabase.co}"

HOST="$(echo "$URL" | sed -E 's#^https?://##; s#/.*$##')"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Consultando ${HOST}…"
if ! echo | openssl s_client -connect "$HOST:443" -servername "$HOST" -showcerts \
       > "$TMP/chain.txt" 2>/dev/null; then
  echo "No se pudo conectar a $HOST:443" >&2
  exit 1
fi

python3 - "$TMP" <<'PY'
import re, sys, subprocess, os
tmp = sys.argv[1]
txt = open(os.path.join(tmp, "chain.txt")).read()
certs = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----", txt, re.S)
if not certs:
    print("La cadena no trae certificados", file=sys.stderr); sys.exit(1)
for i, c in enumerate(certs):
    open(os.path.join(tmp, f"c{i}.pem"), "w").write(c + "\n")
print(f"{len(certs)} certificados en la cadena")
PY

ULTIMO="$(ls "$TMP"/c*.pem | sort | tail -1)"
echo "Raíz de la cadena:"
openssl x509 -in "$ULTIMO" -noout -subject -issuer -enddate | sed 's/^/  /'

# ISRG Root X1: ancla clásica de Let's Encrypt, con la que valida la cadena
# actual de Supabase. Se descarga del origen oficial, no de la cadena servida.
echo "Descargando ISRG Root X1…"
curl -sS -o "$TMP/x1.pem" https://letsencrypt.org/certs/isrgrootx1.pem

HUELLA_ESPERADA="96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6"
HUELLA="$(openssl x509 -in "$TMP/x1.pem" -noout -fingerprint -sha256 | cut -d= -f2)"
if [ "$HUELLA" != "$HUELLA_ESPERADA" ]; then
  echo "⚠ La huella de ISRG Root X1 NO coincide con la conocida." >&2
  echo "  esperada: $HUELLA_ESPERADA" >&2
  echo "  obtenida: $HUELLA" >&2
  echo "  Se aborta: embeber una raíz no verificada anula la seguridad de TLS." >&2
  exit 1
fi
echo "Huella de ISRG Root X1 verificada."

python3 - "$TMP" "$ULTIMO" "$SALIDA" "$HOST" <<'PY'
import sys, subprocess, os
tmp, ultimo, salida, host = sys.argv[1:5]

def info(p):
    def campo(*args):
        return subprocess.run(["openssl","x509","-in",p,"-noout",*args],
                              capture_output=True, text=True).stdout.strip()
    return (campo("-subject").replace("subject=","").strip(),
            campo("-enddate").replace("notAfter=","").strip())

anclas = [(os.path.join(tmp,"x1.pem"), "ancla principal: valida la cadena actual")]
# La raíz servida se incluye también: si mañana el servidor deja de enviar el
# certificado con firma cruzada, esta ancla mantiene la validación.
if os.path.basename(ultimo) != "c0.pem":
    anclas.append((ultimo, "raíz servida en la cadena (respaldo)"))

partes = []
for ruta, nota in anclas:
    sub, exp = info(ruta)
    pem = open(ruta).read().strip()
    lineas = "\n".join(f'    "{l}\\n"' for l in pem.splitlines())
    partes.append(f"    // {sub}\n    // {nota}\n    // expira: {exp}\n{lineas}")

cuerpo = "\n    \"\\n\"\n".join(partes)

open(salida, "w").write(f'''// ============================================================================
// Anclas de confianza TLS — GENERADO AUTOMÁTICAMENTE, no editar a mano
// ============================================================================
// Generado por tools/scripts/generar-ca.sh contra {host}
//
// El equipo valida el certificado del servidor contra estas raíces. NUNCA se
// usa setInsecure(): sin validación, cualquiera en la red podría suplantar al
// servidor y recibir el token de ingesta del equipo.
//
// ⚠ REVISAR LAS FECHAS DE EXPIRACIÓN en el mantenimiento trimestral. Cuando la
// autoridad rote su raíz, el equipo dejará de publicar con un error de TLS y
// desde planta parecerá que se cayó internet. Regenerar este archivo y cargarlo
// por OTA antes de esa fecha.
//
// Regenerar:  ./tools/scripts/generar-ca.sh https://TU_PROYECTO.supabase.co
// ============================================================================

#pragma once

namespace red {{

// mbedtls acepta varias raíces concatenadas en un mismo PEM y prueba todas.
static const char CA_RAICES[] =
{cuerpo};

}}  // namespace red
''')
print(f"Escrito {salida}")
PY

echo
echo "✓ Listo. Recompila el firmware para incorporar los cambios."
