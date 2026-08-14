#!/usr/bin/env bash
# ============================================================================
# Genera firmware/src/red/certificados.h a partir del servidor REAL.
#
#   ./tools/scripts/generar-ca.sh https://TU_PROYECTO.supabase.co
#
# POR QUÉ ESTE SCRIPT EXISTE
# La raíz que firma el certificado NO es la misma para todos los proyectos de
# Supabase: supabase.co usa Let's Encrypt, y un proyecto concreto puede usar
# Google Trust Services. Fijar una raíz "conocida" a ojo hace que el equipo
# falle la validación TLS y deje de publicar, y desde planta eso parece
# simplemente "se cayó internet".
#
# POR QUÉ NO BASTA CON COPIAR EL ÚLTIMO CERTIFICADO DE LA CADENA
# El servidor suele enviar la raíz en su versión CON FIRMA CRUZADA, emitida por
# otra autoridad más antigua. Ese certificado NO sirve como ancla de confianza:
# no es auto-firmado, así que la validación intenta buscar A SU emisor y falla.
# Hay que embeber la versión AUTO-FIRMADA, que se toma del almacén del sistema.
#
# El script verifica la cadena contra lo que va a embeber ANTES de escribir el
# archivo. Si no valida, aborta: es preferible no generar nada a generar unas
# anclas que dejarían el equipo mudo.
# ============================================================================
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SALIDA="$RAIZ/firmware/src/red/certificados.h"
URL="${1:-https://supabase.co}"

HOST="$(echo "$URL" | sed -E 's#^https?://##; s#/.*$##')"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Almacén de confianza del sistema, del que se extraen las raíces auto-firmadas.
ALMACEN=""
for c in /etc/ssl/cert.pem /usr/local/etc/openssl/cert.pem \
         /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt; do
  [ -r "$c" ] && ALMACEN="$c" && break
done
if [ -z "$ALMACEN" ]; then
  echo "No se encontró el almacén de certificados del sistema." >&2
  exit 1
fi

echo "Consultando ${HOST}…"
if ! echo | openssl s_client -connect "$HOST:443" -servername "$HOST" -showcerts \
       > "$TMP/chain.txt" 2>/dev/null; then
  echo "No se pudo conectar a $HOST:443" >&2
  exit 1
fi

python3 - "$TMP" "$ALMACEN" "$SALIDA" "$HOST" <<'PY'
import re, os, sys, subprocess

tmp, almacen, salida, host = sys.argv[1:5]

def certs_de(texto):
    return re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
                      texto, re.S)

def campo(pem_path, *args):
    return subprocess.run(["openssl", "x509", "-in", pem_path, "-noout", *args],
                          capture_output=True, text=True).stdout.strip()

cadena = certs_de(open(os.path.join(tmp, "chain.txt")).read())
if not cadena:
    print("La cadena no trae certificados", file=sys.stderr); sys.exit(1)

for i, c in enumerate(cadena):
    open(os.path.join(tmp, f"c{i}.pem"), "w").write(c + "\n")
print(f"{len(cadena)} certificados en la cadena servida:")
for i in range(len(cadena)):
    print("  " + campo(os.path.join(tmp, f"c{i}.pem"), "-subject"))

ultimo = os.path.join(tmp, f"c{len(cadena)-1}.pem")
sub_ultimo = campo(ultimo, "-subject").replace("subject=", "").strip()
iss_ultimo = campo(ultimo, "-issuer").replace("issuer=", "").strip()
autofirmado = (sub_ultimo == iss_ultimo)

# ---------------------------------------------------------------------------
# Elegir las anclas
# ---------------------------------------------------------------------------
# Se buscan en el almacén del sistema las raíces AUTO-FIRMADAS que cierran esta
# cadena concreta. Se toman dos cuando existen:
#   · la raíz propia del último certificado servido (si es cruzado, su versión
#     auto-firmada), para el día en que el servidor deje de enviar el cruzado;
#   · la raíz que EMITIÓ ese certificado cruzado, que es la que valida la cadena
#     tal como se sirve hoy.
buscados = {sub_ultimo}
if not autofirmado:
    buscados.add(iss_ultimo)
    print(f"\nEl último certificado servido NO es auto-firmado:")
    print(f"  sujeto : {sub_ultimo}")
    print(f"  emisor : {iss_ultimo}")
    print("  -> se buscarán ambas raíces auto-firmadas en el almacén del sistema")

sistema = certs_de(open(almacen).read())
anclas = []
vistos = set()
for pem in sistema:
    p = os.path.join(tmp, "cand.pem")
    open(p, "w").write(pem + "\n")
    s = campo(p, "-subject").replace("subject=", "").strip()
    i = campo(p, "-issuer").replace("issuer=", "").strip()
    if s in buscados and s == i and s not in vistos:   # auto-firmada y buscada
        vistos.add(s)
        fin = campo(p, "-enddate").replace("notAfter=", "").strip()
        ruta = os.path.join(tmp, f"ancla{len(anclas)}.pem")
        open(ruta, "w").write(pem + "\n")
        anclas.append((ruta, s, fin))

if not anclas:
    print("\nNo se encontró ninguna raíz auto-firmada aplicable en el almacén "
          "del sistema. Abortando.", file=sys.stderr)
    sys.exit(1)

print("\nAnclas seleccionadas (auto-firmadas):")
for _, s, fin in anclas:
    print(f"  {s}\n    expira: {fin}")

# ---------------------------------------------------------------------------
# VERIFICACIÓN OBLIGATORIA antes de escribir nada
# ---------------------------------------------------------------------------
paquete = os.path.join(tmp, "anclas.pem")
open(paquete, "w").write("\n".join(open(r).read().strip() for r, _, _ in anclas) + "\n")

inter = os.path.join(tmp, "inter.pem")
open(inter, "w").write("\n".join(
    open(os.path.join(tmp, f"c{i}.pem")).read().strip()
    for i in range(1, len(cadena))) + "\n")

r = subprocess.run(["openssl", "verify", "-CAfile", paquete,
                    "-untrusted", inter, os.path.join(tmp, "c0.pem")],
                   capture_output=True, text=True)
print("\nVerificación de la cadena real contra estas anclas:")
print("  " + (r.stdout.strip() or r.stderr.strip()).replace("\n", "\n  "))
if r.returncode != 0:
    print("\n  ✗ La cadena NO valida. No se escribe el archivo: unas anclas\n"
          "    incorrectas dejarían al equipo sin poder publicar.", file=sys.stderr)
    sys.exit(1)
print("  ✓ La cadena valida correctamente")

# ---------------------------------------------------------------------------
partes = []
for ruta, s, fin in anclas:
    pem = open(ruta).read().strip()
    lineas = "\n".join(f'    "{l}\\n"' for l in pem.splitlines())
    partes.append(f"    // {s}\n    // expira: {fin}\n{lineas}")
cuerpo = "\n    \"\\n\"\n".join(partes)

open(salida, "w").write(f'''// ============================================================================
// Anclas de confianza TLS — GENERADO AUTOMÁTICAMENTE, no editar a mano
// ============================================================================
// Generado por tools/scripts/generar-ca.sh contra {host}
//
// El equipo valida el certificado del servidor contra estas raíces. NUNCA se
// usa setInsecure(): sin validación, cualquiera en la red podría suplantar al
// servidor y quedarse con el token de ingesta del equipo.
//
// Son las raíces AUTO-FIRMADAS que cierran la cadena de ESTE proyecto, tomadas
// del almacén de confianza del sistema. No se copian de la cadena servida: el
// servidor suele enviar la raíz con firma cruzada, que no sirve como ancla.
// El script verifica la cadena real contra ellas antes de escribir este archivo.
//
// ⚠ REVISAR LAS FECHAS DE EXPIRACIÓN en el mantenimiento trimestral. Cuando la
// autoridad rote su raíz, el equipo dejará de publicar con un error de TLS y
// desde planta parecerá que se cayó internet. Regenerar este archivo y cargarlo
// por OTA antes de esa fecha.
//
// Regenerar:  ./tools/scripts/generar-ca.sh {host and "https://" + host}
// ============================================================================

#pragma once

namespace red {{

// mbedtls acepta varias raíces concatenadas en un mismo PEM y prueba todas.
static const char CA_RAICES[] =
{cuerpo};

}}  // namespace red
''')
print(f"\nEscrito {salida}")
PY

echo
echo "✓ Listo. Recompila el firmware para incorporar los cambios."
