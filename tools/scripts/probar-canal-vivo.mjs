#!/usr/bin/env node
// ============================================================================
// Prueba de seguridad del canal de tiempo real
// ============================================================================
//   node tools/scripts/probar-canal-vivo.mjs
//
// Responde a UNA pregunta: ¿puede alguien que solo tiene la anon key —que es
// pública, va incrustada en el dashboard y cualquiera puede leer del bundle—
// inyectar lecturas falsas en el canal en vivo?
//
// Si la respuesta fuera sí, el dashboard podría mostrar 200 °C sin que nada
// esté caliente, y no habría forma de distinguirlo de un dato real.
//
// NO basta con mirar el código de estado HTTP: el endpoint de broadcast
// responde 202 ("encolado") antes de evaluar la autorización, así que un 202 no
// prueba que el mensaje se haya entregado. Esta prueba SE SUSCRIBE al canal y
// comprueba qué llega de verdad.
// ============================================================================

import { readFileSync, existsSync } from "node:fs";
import { createClient } from "../../web/node_modules/@supabase/supabase-js/dist/index.mjs";

function cargarEnv(ruta = ".env") {
  if (!existsSync(ruta)) return;
  for (const linea of readFileSync(ruta, "utf8").split("\n")) {
    const m = linea.match(/^\s*([A-Z0-9_]+)\s*=\s*(.*)$/);
    if (m && !process.env[m[1]]) process.env[m[1]] = m[2].trim().replace(/^["']|["']$/g, "");
  }
}
cargarEnv();

const URL = process.env.SUPABASE_URL;
const ANON = process.env.SUPABASE_ANON_KEY;
const JWT = process.env.DEVICE_JWT;
const SLUG = process.env.DEVICE_SLUG ?? "planta-01";
const TOPIC = `telemetria:${SLUG}`;

if (!URL || !ANON || !JWT) {
  console.error("Faltan SUPABASE_URL, SUPABASE_ANON_KEY o DEVICE_JWT en .env");
  process.exit(1);
}

// --- Suscriptor: exactamente lo que hace el dashboard ------------------------
const cliente = createClient(URL, ANON);
const recibidos = [];

const canal = cliente
  .channel(TOPIC, { config: { private: true } })
  .on("broadcast", { event: "lectura" }, ({ payload }) => {
    recibidos.push(payload);
    console.log(`    ← recibido: marca=${payload?.marca ?? "?"}`);
  });

const suscrito = await new Promise((res) => {
  canal.subscribe((estado, err) => {
    if (estado === "SUBSCRIBED") res(true);
    if (["CHANNEL_ERROR", "TIMED_OUT", "CLOSED"].includes(estado)) {
      console.error(`  suscripción falló: ${estado}`, err?.message ?? "");
      res(false);
    }
  });
});

if (!suscrito) {
  console.error("\nNo se pudo suscribir al canal. Abortando.");
  process.exit(1);
}
console.log(`Suscrito a ${TOPIC} con la anon key (como el dashboard)\n`);

// --- Emisores ---------------------------------------------------------------
async function emitir(etiqueta, autorizacion, marca) {
  const r = await fetch(`${URL}/realtime/v1/api/broadcast`, {
    method: "POST",
    headers: {
      apikey: ANON,
      Authorization: `Bearer ${autorizacion}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      messages: [{
        topic: TOPIC,
        event: "lectura",
        private: true,
        payload: { marca, ts: new Date().toISOString(), peso_g: 1234.5, faults: 0 },
      }],
    }),
  });
  console.log(`  ${etiqueta}: HTTP ${r.status}`);
  return r.status;
}

const espera = (ms) => new Promise((r) => setTimeout(r, ms));

console.log("Emitiendo tres mensajes con distinta autorización:\n");

const httpLegitimo = await emitir("equipo legítimo (device JWT)", JWT, "LEGITIMO");
await espera(2500);

const httpAnon = await emitir("intruso con la anon key       ", ANON, "FALSO_ANON");
await espera(2500);

// JWT válido pero de otro equipo: no debe poder publicar en este canal.
const { createHmac } = await import("node:crypto");
const secreto = process.env.SUPABASE_JWT_SECRET;
let httpOtro = null;
if (secreto) {
  const b64 = (o) => Buffer.from(JSON.stringify(o)).toString("base64url");
  const ahora = Math.floor(Date.now() / 1000);
  const cuerpo = `${b64({ alg: "HS256", typ: "JWT" })}.${b64({
    role: "authenticated", aud: "authenticated", sub: "device:otra-planta",
    device_slug: "otra-planta", iat: ahora, exp: ahora + 3600,
  })}`;
  const otro = `${cuerpo}.${createHmac("sha256", secreto).update(cuerpo).digest("base64url")}`;
  httpOtro = await emitir("otro equipo (JWT de otra-planta)", otro, "FALSO_OTRO_EQUIPO");
  await espera(2500);
}

// --- Veredicto --------------------------------------------------------------
const llego = (m) => recibidos.some((p) => p?.marca === m);

console.log("\n" + "─".repeat(64));
console.log("VEREDICTO — qué llegó realmente al suscriptor\n");

const casos = [
  ["El equipo legítimo publica", "LEGITIMO", true, httpLegitimo],
  ["La anon key NO puede publicar", "FALSO_ANON", false, httpAnon],
];
if (httpOtro !== null) {
  casos.push(["Otro equipo NO puede publicar en este canal", "FALSO_OTRO_EQUIPO", false, httpOtro]);
}

let fallos = 0;
for (const [desc, marca, esperado, http] of casos) {
  const recibido = llego(marca);
  const ok = recibido === esperado;
  if (!ok) fallos++;
  console.log(`  ${ok ? "\x1b[32m✓\x1b[0m" : "\x1b[31m✗\x1b[0m"} ${desc}`);
  console.log(`      HTTP ${http} · ${recibido ? "SÍ se entregó" : "no se entregó"}` +
              (ok ? "" : `  ← se esperaba ${esperado ? "que llegara" : "que NO llegara"}`));
}

console.log("─".repeat(64));
if (fallos === 0) {
  console.log("\x1b[32mCanal seguro: solo el equipo dueño del topic puede publicar.\x1b[0m\n");
} else {
  console.log("\x1b[31mCANAL INSEGURO: cualquiera con el enlace del dashboard podría\x1b[0m");
  console.log("\x1b[31minyectar lecturas falsas. NO usar el tiempo real hasta corregirlo.\x1b[0m\n");
}

await cliente.removeChannel(canal);
process.exit(fallos === 0 ? 0 : 1);
