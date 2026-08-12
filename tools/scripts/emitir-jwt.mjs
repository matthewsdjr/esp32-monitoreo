#!/usr/bin/env node
// ============================================================================
// Emite el JWT de dispositivo para publicar en el canal Realtime (Ruta A).
//
//   node tools/scripts/emitir-jwt.mjs planta-01 [años]
//
// Requiere SUPABASE_JWT_SECRET (Dashboard → Settings → API → JWT Secret).
//
// POR QUÉ EXISTE ESTE TOKEN:
// La anon key es pública —va incrustada en el dashboard—. Si el ESP32 publicara
// con ella, cualquiera que abriera la página tendría la credencial para inyectar
// lecturas falsas en el canal, y el dashboard mostraría 200 °C sin que nada esté
// caliente. Este JWT lleva el claim `device_slug`, y la política RLS sobre
// realtime.messages (migración 0002) solo permite publicar en el topic que
// coincide con ese claim.
// ============================================================================

import { createHmac } from "node:crypto";
import { readFileSync, existsSync } from "node:fs";

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

const SECRETO = process.env.SUPABASE_JWT_SECRET;
if (!SECRETO) {
  console.error("Falta SUPABASE_JWT_SECRET (Dashboard → Settings → API → JWT Secret)");
  process.exit(1);
}

const slug = process.argv[2];
const anios = Number(process.argv[3] ?? 5);

if (!slug) {
  console.error("Uso: node emitir-jwt.mjs <device-slug> [años de vigencia]");
  process.exit(1);
}

const b64 = (obj) => Buffer.from(JSON.stringify(obj)).toString("base64url");

const ahora = Math.floor(Date.now() / 1000);
const exp = ahora + Math.round(anios * 365.25 * 86400);

const cabecera = { alg: "HS256", typ: "JWT" };
const claims = {
  // `authenticated` es el rol que exige la política de INSERT sobre
  // realtime.messages. No otorga ningún privilegio de tabla: RLS no define
  // ninguna política de escritura para ese rol en el esquema public.
  role: "authenticated",
  aud: "authenticated",
  sub: `device:${slug}`,
  device_slug: slug, // ← lo que la política compara contra el topic
  iat: ahora,
  exp,
};

const cuerpo = `${b64(cabecera)}.${b64(claims)}`;
const firma = createHmac("sha256", SECRETO).update(cuerpo).digest("base64url");
const jwt = `${cuerpo}.${firma}`;

const vence = new Date(exp * 1000).toISOString().slice(0, 10);

console.log(`
════════════════════════════════════════════════════════════════
  JWT de dispositivo · ${slug}
════════════════════════════════════════════════════════════════
  topic autorizado   telemetria:${slug}
  vence              ${vence}

${jwt}

────────────────────────────────────────────────────────────────
  Guárdalo en la NVS del ESP32. NUNCA en el repositorio.

  RECORDATORIO: anota la fecha de vencimiento. Cuando expire, el
  equipo dejará de publicar en tiempo real (la Ruta B de ingesta
  seguirá funcionando, así que el histórico no se pierde, pero el
  dashboard dejará de actualizarse en vivo). Reemitir y cargar por
  OTA antes de esa fecha.
════════════════════════════════════════════════════════════════
`);
