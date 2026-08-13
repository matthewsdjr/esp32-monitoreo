#!/usr/bin/env node
// ============================================================================
// Fija el PIN de operador que habilita los comandos remotos (tara, calibración).
//
//   node tools/scripts/fijar-pin-operador.mjs 4821
//   node tools/scripts/fijar-pin-operador.mjs --desactivar
//
// El PIN NO se guarda: en la base solo queda su SHA-256. Si se olvida, se fija
// uno nuevo. Mientras el hash esté vacío, /comando rechaza TODO — que es el
// estado por defecto de un despliegue recién hecho.
// ============================================================================

import { createHash } from "node:crypto";
import { readFileSync, existsSync } from "node:fs";

function cargarEnv(ruta = ".env") {
  if (!existsSync(ruta)) return;
  for (const linea of readFileSync(ruta, "utf8").split("\n")) {
    const m = linea.match(/^\s*([A-Z0-9_]+)\s*=\s*(.*)$/);
    if (m && !process.env[m[1]]) process.env[m[1]] = m[2].trim().replace(/^["']|["']$/g, "");
  }
}
cargarEnv();

const URL = process.env.SUPABASE_URL;
const CLAVE = process.env.SUPABASE_SERVICE_ROLE_KEY;
if (!URL || !CLAVE) {
  console.error("Falta SUPABASE_URL o SUPABASE_SERVICE_ROLE_KEY");
  process.exit(1);
}

const arg = process.argv[2];
if (!arg) {
  console.error("Uso: node fijar-pin-operador.mjs <PIN>   |   --desactivar");
  process.exit(1);
}

const desactivar = arg === "--desactivar";
const pin = desactivar ? "" : arg;

if (!desactivar) {
  if (pin.length < 4) {
    console.error("El PIN debe tener al menos 4 caracteres.");
    process.exit(1);
  }
  // Un PIN de 4 dígitos son 10 000 combinaciones. Con el límite de 5 intentos
  // por 15 min de /comando, agotarlas tomaría décadas — pero un PIN más largo
  // no cuesta nada y elimina la discusión.
  if (/^\d{4}$/.test(pin)) {
    console.warn("⚠  PIN de 4 dígitos. Es aceptable por el límite de intentos,");
    console.warn("   pero 6+ caracteres alfanuméricos es notablemente mejor.\n");
  }
  const obvios = ["1234", "0000", "1111", "12345", "123456", "0123"];
  if (obvios.includes(pin)) {
    console.error("Ese PIN es de los primeros que probaría cualquiera. Elige otro.");
    process.exit(1);
  }
}

const hash = desactivar ? "" : createHash("sha256").update(pin).digest("hex");

const r = await fetch(`${URL}/rest/v1/app_config?clave=eq.operador_pin_hash`, {
  method: "PATCH",
  headers: {
    apikey: CLAVE,
    Authorization: `Bearer ${CLAVE}`,
    "Content-Type": "application/json",
    Prefer: "return=representation",
  },
  body: JSON.stringify({ valor: hash }),
});

if (!r.ok) {
  console.error(`Error ${r.status}:`, await r.text());
  process.exit(1);
}

const filas = await r.json();
if (filas.length === 0) {
  console.error("No existe la clave operador_pin_hash. ¿Aplicaste la migración 0005?");
  process.exit(1);
}

console.log(
  desactivar
    ? "\n✓ Comandos remotos DESACTIVADOS. /comando rechazará toda petición.\n"
    : `\n✓ PIN de operador actualizado.

  Compártelo solo con quien deba poder tarar la báscula.
  No queda guardado en ningún lado: si se olvida, se fija uno nuevo.
`,
);
