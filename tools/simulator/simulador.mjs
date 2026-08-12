#!/usr/bin/env node
// ============================================================================
// Simulador del ESP32
// ============================================================================
// Emula exactamente lo que hará el firmware, por las DOS rutas:
//   Ruta A · broadcast a Realtime cada 2 s   (tiempo real)
//   Ruta B · POST por lotes a /ingest cada 30 s (histórico)
//
// Existe para desacoplar el desarrollo del dashboard del hardware: la Fase 4
// se construye entera contra esto, sin ESP32 presente. Cuando el equipo real
// entre en línea, el dashboard no cambia una sola línea.
//
//   node tools/simulator/simulador.mjs
//   node tools/simulator/simulador.mjs --escenario=horno
//   node tools/simulator/simulador.mjs --escenario=falla-tc1
//   node tools/simulator/simulador.mjs --escenario=caida-red
//
// Requiere en .env:  SUPABASE_URL, DEVICE_SLUG, DEVICE_TOKEN, DEVICE_JWT, SUPABASE_ANON_KEY
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

const URL = process.env.SUPABASE_URL;
const SLUG = process.env.DEVICE_SLUG ?? "planta-01";
const TOKEN = process.env.DEVICE_TOKEN;
const JWT = process.env.DEVICE_JWT;
const ANON = process.env.SUPABASE_ANON_KEY;

if (!URL || !TOKEN) {
  console.error("Falta SUPABASE_URL o DEVICE_TOKEN. Copia .env.example a .env y complétalo.");
  process.exit(1);
}

const args = Object.fromEntries(
  process.argv.slice(2).map((a) => {
    const [k, v] = a.replace(/^--/, "").split("=");
    return [k, v ?? true];
  }),
);
const ESCENARIO = args.escenario ?? "normal";

// Cadencias — idénticas a las del firmware. Ver docs/ARQUITECTURA.md §6.
const MS_MUESTREO = 5_000;   // una muestra cada 5 s
const MS_BROADCAST = 2_000;  // tiempo real cada 2 s
const MS_LOTE = 30_000;      // subida por lotes cada 30 s

// ----------------------------------------------------------------------------
// Modelo físico
// ----------------------------------------------------------------------------
// Los valores no son aleatorios puros: siguen un paseo aleatorio con retorno a
// la media más ruido. Un `Math.random()` plano produce gráficas que no se
// parecen en nada a un proceso real y deja pasar errores de escala y suavizado
// en el dashboard.
function crearCanal({ base, deriva, ruido, min, max }) {
  let v = base;
  return () => {
    v += (base - v) * deriva + (Math.random() - 0.5) * ruido;
    return Math.min(max, Math.max(min, v));
  };
}

const canales = {
  peso: crearCanal({ base: 1250, deriva: 0.02, ruido: 6, min: 0, max: 50000 }),
  temp_amb: crearCanal({ base: 22.5, deriva: 0.05, ruido: 0.3, min: -40, max: 125 }),
  hum: crearCanal({ base: 52, deriva: 0.05, ruido: 1.2, min: 0, max: 100 }),
  tc1: crearCanal({ base: 178, deriva: 0.03, ruido: 2.5, min: 0, max: 1024 }),
  tc2: crearCanal({ base: 172, deriva: 0.03, ruido: 2.5, min: 0, max: 1024 }),
};

const BIT_FALLA = { peso: 0, temp_amb: 1, hum: 2, tc1: 3, tc2: 4 };

const arranque = Date.now();
let reconexiones = 0;

function generarMuestra() {
  const t = (Date.now() - arranque) / 1000;
  let m = {
    ts: new Date().toISOString(),
    peso_g: +canales.peso().toFixed(1),
    temp_amb_c: +canales.temp_amb().toFixed(2),
    hum_pct: +canales.hum().toFixed(1),
    tc1_c: +canales.tc1().toFixed(2),
    tc2_c: +canales.tc2().toFixed(2),
    faults: 0,
  };

  switch (ESCENARIO) {
    case "horno":
      // Rampa que cruza warn_high (200) y luego alarm_high (250): sirve para
      // ver la escalada de advertencia -> alarma en el dashboard.
      m.tc1_c = +(150 + Math.min(130, t * 1.2)).toFixed(2);
      break;

    case "falla-tc1":
      // Termopar abierto a partir del segundo 60.
      if (t > 60) {
        m.tc1_c = null;
        m.faults |= 1 << BIT_FALLA.tc1;
      }
      break;

    case "deriva-bascula":
      m.peso_g = +(1250 + t * 0.8).toFixed(1);
      break;
  }
  return m;
}

// ----------------------------------------------------------------------------
// Ruta A · broadcast de tiempo real
// ----------------------------------------------------------------------------
// Se usa el endpoint HTTP de Realtime en vez de abrir un WebSocket con el
// protocolo Phoenix. Es una decisión que se traslada al firmware: implementar
// Phoenix (canales, heartbeats, reconexión) sobre un ESP32 es mucho código
// frágil, mientras que un POST con keep-alive es trivial y consume lo mismo en
// cuota de Realtime.
async function transmitirEnVivo(muestra) {
  if (!JWT) return; // sin JWT no hay tiempo real, pero el histórico sigue

  const r = await fetch(`${URL}/realtime/v1/api/broadcast`, {
    method: "POST",
    headers: {
      apikey: ANON ?? "",
      Authorization: `Bearer ${JWT}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      messages: [{
        topic: `telemetria:${SLUG}`,
        event: "lectura",
        payload: muestra,
        private: true, // exige la política RLS de la migración 0002
      }],
    }),
  });
  if (!r.ok) console.error("  broadcast falló", r.status, await r.text());
}

// ----------------------------------------------------------------------------
// Ruta B · lote a /ingest
// ----------------------------------------------------------------------------
let bufer = [];

async function subirLote() {
  if (bufer.length === 0) return;

  // Escenario de red caída: el búfer crece, nada se pierde, y al reanudar se
  // drena con las marcas de tiempo ORIGINALES. Es la prueba de que el histórico
  // no queda con huecos.
  if (ESCENARIO === "caida-red") {
    const t = (Date.now() - arranque) / 1000;
    if (t > 45 && t < 150) {
      console.log(`  [red caída] búfer: ${bufer.length} muestras retenidas`);
      return;
    }
    if (t >= 150 && bufer.length > 10) {
      reconexiones++;
      console.log(`  [reconectado] drenando ${bufer.length} muestras`);
    }
  }

  const lote = bufer.slice(0, 120);

  const r = await fetch(`${URL}/functions/v1/ingest`, {
    method: "POST",
    headers: { Authorization: `Bearer ${TOKEN}`, "Content-Type": "application/json" },
    body: JSON.stringify({
      device: SLUG,
      samples: lote,
      health: {
        rssi: -55 - Math.round(Math.random() * 20),
        uptime_s: Math.round((Date.now() - arranque) / 1000),
        free_heap: 180000 + Math.round(Math.random() * 20000),
        reconnects: reconexiones,
        fw: "sim-1.0.0",
      },
    }),
  });

  if (r.ok) {
    const j = await r.json();
    bufer = bufer.slice(lote.length);
    console.log(`  ↑ ${j.recibidas} muestras guardadas (búfer restante: ${bufer.length})`);
  } else {
    // No se vacía el búfer: se reintentará en el siguiente ciclo. Exactamente
    // el comportamiento que tendrá el firmware.
    console.error(`  ↑ falló ${r.status}: ${await r.text()}`);
  }
}

// ----------------------------------------------------------------------------
console.log(`
Simulador ESP32
  equipo      ${SLUG}
  escenario   ${ESCENARIO}
  destino     ${URL}
  tiempo real ${JWT ? "activo (cada 2 s)" : "DESACTIVADO — falta DEVICE_JWT"}
  lotes       cada ${MS_LOTE / 1000} s

Ctrl-C para detener.
`);

let ultima = generarMuestra();

setInterval(() => {
  ultima = generarMuestra();
  bufer.push(ultima);
  if (bufer.length > 8640) bufer.shift(); // tope: 12 h, igual que el firmware
}, MS_MUESTREO);

setInterval(() => {
  transmitirEnVivo(ultima).catch((e) => console.error("  broadcast", e.message));
  const f = ultima.faults ? ` ⚠ faults=${ultima.faults}` : "";
  console.log(
    `  ~ peso ${String(ultima.peso_g).padStart(7)} g` +
    ` · amb ${ultima.temp_amb_c}°C ${ultima.hum_pct}%` +
    ` · tc1 ${ultima.tc1_c ?? "—"}°C · tc2 ${ultima.tc2_c}°C${f}`,
  );
}, MS_BROADCAST);

setInterval(() => {
  subirLote().catch((e) => console.error("  lote", e.message));
}, MS_LOTE);
