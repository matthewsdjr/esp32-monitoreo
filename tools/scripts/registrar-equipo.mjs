#!/usr/bin/env node
// ============================================================================
// Alta de un equipo: genera su token de ingesta y lo registra.
//
//   node tools/scripts/registrar-equipo.mjs planta-01 "Línea 1" "Planta principal"
//
// Requiere en el entorno (.env, NO versionado):
//   SUPABASE_URL, SUPABASE_SERVICE_ROLE_KEY
//
// El token se muestra UNA SOLA VEZ. En la base solo queda su hash SHA-256, así
// que no hay forma de recuperarlo después: si se pierde, se rota.
// ============================================================================

import { randomBytes, createHash } from "node:crypto";
import { readFileSync, existsSync } from "node:fs";

// --- Carga de .env sin dependencias ----------------------------------------
function cargarEnv(ruta = ".env") {
  if (!existsSync(ruta)) return;
  for (const linea of readFileSync(ruta, "utf8").split("\n")) {
    const m = linea.match(/^\s*([A-Z0-9_]+)\s*=\s*(.*)$/);
    if (m && !process.env[m[1]]) {
      process.env[m[1]] = m[2].trim().replace(/^["']|["']$/g, "");
    }
  }
}
cargarEnv();

const URL = process.env.SUPABASE_URL;
const CLAVE = process.env.SUPABASE_SERVICE_ROLE_KEY;

if (!URL || !CLAVE) {
  console.error("Falta SUPABASE_URL o SUPABASE_SERVICE_ROLE_KEY en el entorno o en .env");
  process.exit(1);
}

const [slug, nombre, ubicacion] = process.argv.slice(2);

if (!slug || !/^[a-z0-9][a-z0-9-]{1,38}[a-z0-9]$/.test(slug)) {
  console.error("Uso: node registrar-equipo.mjs <slug> [nombre] [ubicacion]");
  console.error("  slug: minúsculas, dígitos y guiones. Ej: planta-01");
  process.exit(1);
}

// 32 bytes = 256 bits de entropía criptográfica. Por eso basta SHA-256 para
// almacenarlo y no hace falta un KDF lento: no es fuerza-bruteable.
const token = randomBytes(32).toString("base64url");
const hash = createHash("sha256").update(token).digest("hex");

const cuerpo = {
  slug,
  nombre: nombre || slug,
  ubicacion: ubicacion || null,
  token_hash: hash,
  token_rotado_at: new Date().toISOString(),
};

const r = await fetch(`${URL}/rest/v1/devices?on_conflict=slug`, {
  method: "POST",
  headers: {
    apikey: CLAVE,
    Authorization: `Bearer ${CLAVE}`,
    "Content-Type": "application/json",
    Prefer: "resolution=merge-duplicates,return=representation",
  },
  body: JSON.stringify(cuerpo),
});

if (!r.ok) {
  console.error(`Error ${r.status}:`, await r.text());
  process.exit(1);
}

const [equipo] = await r.json();

console.log(`
════════════════════════════════════════════════════════════════
  Equipo registrado
════════════════════════════════════════════════════════════════
  slug       ${equipo.slug}
  id         ${equipo.id}
  nombre     ${equipo.nombre}

  TOKEN DE INGESTA (se muestra una sola vez):

    ${token}

────────────────────────────────────────────────────────────────
  Guárdalo en la NVS del ESP32, NUNCA en el repositorio.

  Siguiente paso — emitir el JWT para el canal de tiempo real:
    node tools/scripts/emitir-jwt.mjs ${equipo.slug}
════════════════════════════════════════════════════════════════
`);
