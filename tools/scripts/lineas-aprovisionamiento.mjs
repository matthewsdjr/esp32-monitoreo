#!/usr/bin/env node
// ============================================================================
// Imprime las líneas exactas para aprovisionar el ESP32 por puerto serie.
//
//   node tools/scripts/lineas-aprovisionamiento.mjs "MiRedWiFi" "MiContraseña"
//
// Se copian y se pegan tal cual en el monitor serie. Evita transcribir a mano
// un JWT de 256 caracteres, que es donde se cometen los errores.
//
// ⚠ La salida CONTIENE SECRETOS. No la pegues en un chat, un ticket ni un
//   documento compartido: solo en el monitor serie del equipo.
// ============================================================================

import { readFileSync, existsSync } from "node:fs";

function cargarEnv(ruta = ".env") {
  if (!existsSync(ruta)) return;
  for (const linea of readFileSync(ruta, "utf8").split("\n")) {
    const m = linea.match(/^\s*([A-Z0-9_]+)\s*=\s*(.*)$/);
    if (m && !process.env[m[1]]) process.env[m[1]] = m[2].trim().replace(/^["']|["']$/g, "");
  }
}
cargarEnv();

const [ssid, pass] = process.argv.slice(2);

const url = process.env.SUPABASE_URL;
const anon = process.env.SUPABASE_ANON_KEY;
const slug = process.env.DEVICE_SLUG ?? "planta-01";
const token = process.env.DEVICE_TOKEN;
const jwt = process.env.DEVICE_JWT;

const falta = [];
if (!url || url.includes("TU_")) falta.push("SUPABASE_URL");
if (!token || token.includes("TU_")) falta.push("DEVICE_TOKEN");
if (falta.length) {
  console.error(`Faltan en .env: ${falta.join(", ")}`);
  process.exit(1);
}

if (!ssid) {
  console.error("\nUso: node tools/scripts/lineas-aprovisionamiento.mjs <red-wifi> <contraseña>\n");
  console.error("Ejemplo:  node tools/scripts/lineas-aprovisionamiento.mjs \"MiRed\" \"micontraseña\"\n");
  process.exit(1);
}

const lineas = [
  `set ssid ${ssid}`,
  `set pass ${pass ?? ""}`,
  `set url ${url}`,
  `set slug ${slug}`,
  `set token ${token}`,
];
if (jwt && !jwt.includes("TU_")) lineas.push(`set jwt ${jwt}`);
if (anon && !anon.includes("TU_")) lineas.push(`set anon ${anon}`);

console.log(`
════════════════════════════════════════════════════════════════
  Pega estas líneas en el monitor serie, UNA POR UNA
════════════════════════════════════════════════════════════════
`);
console.log(lineas.join("\n"));
console.log(`
────────────────────────────────────────────────────────────────
  Luego:
    ver    para revisar (los secretos salen enmascarados)
    r      para reiniciar y conectar

  ⚠ Estas líneas contienen los secretos del equipo. No las pegues
    en un chat, un ticket ni un documento compartido.
════════════════════════════════════════════════════════════════
`);
