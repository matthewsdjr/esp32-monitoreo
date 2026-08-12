#!/usr/bin/env bash
# ============================================================================
# Valida las migraciones contra un Postgres real, desechable.
#
#   ./tools/scripts/test-db.sh
#
# Levanta un contenedor, aplica stubs + migraciones + pruebas funcionales, y lo
# destruye. No toca el proyecto de Supabase ni ningún dato real.
# ============================================================================
set -euo pipefail

RAIZ="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONTENEDOR="monitoreo-db-test"
IMAGEN="postgres:15-alpine"
PGPASS="test"

rojo()  { printf '\033[31m%s\033[0m\n' "$*"; }
verde() { printf '\033[32m%s\033[0m\n' "$*"; }
gris()  { printf '\033[90m%s\033[0m\n' "$*"; }

limpiar() { docker rm -f "$CONTENEDOR" >/dev/null 2>&1 || true; }
trap limpiar EXIT

limpiar
gris "Levantando $IMAGEN..."
docker run -d --name "$CONTENEDOR" \
  -e POSTGRES_PASSWORD="$PGPASS" -e POSTGRES_DB=monitoreo \
  "$IMAGEN" >/dev/null

for _ in $(seq 1 60); do
  if docker exec "$CONTENEDOR" pg_isready -U postgres -d monitoreo >/dev/null 2>&1; then break; fi
  sleep 1
done
docker exec "$CONTENEDOR" pg_isready -U postgres -d monitoreo >/dev/null

ejecutar() {
  local etiqueta="$1" archivo="$2"
  gris "  → $etiqueta"
  if ! docker exec -i "$CONTENEDOR" \
       psql -U postgres -d monitoreo -v ON_ERROR_STOP=1 -q < "$archivo"; then
    rojo "FALLÓ: $etiqueta"
    exit 1
  fi
}

gris "Aplicando stubs..."
ejecutar "00_stubs.sql" "$RAIZ/supabase/tests/00_stubs.sql"

gris "Aplicando migraciones..."
TMP="$(mktemp -d)"
for m in "$RAIZ"/supabase/migrations/*.sql; do
  # pg_cron y pg_net no existen en la imagen oficial de Postgres; sus objetos
  # ya están emulados en 00_stubs.sql, así que se omite su instalación.
  sed -E 's/^[[:space:]]*create extension if not exists (pg_cron|pg_net);/-- (stub) &/I' \
    "$m" > "$TMP/$(basename "$m")"
  ejecutar "$(basename "$m")" "$TMP/$(basename "$m")"
done

gris "Aplicando semilla..."
ejecutar "seed.sql" "$RAIZ/supabase/seed.sql"

gris "Ejecutando pruebas funcionales..."
ejecutar "10_pruebas.sql" "$RAIZ/supabase/tests/10_pruebas.sql"

rm -rf "$TMP"
verde ""
verde "✓ Migraciones y pruebas OK"
