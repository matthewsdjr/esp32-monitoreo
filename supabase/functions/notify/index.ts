// ============================================================================
// Edge Function · POST /notify
// ============================================================================
// La invoca el trigger de alarmas vía pg_net cuando se abre un evento.
// No es pública: exige el secreto compartido de app_config.notify_secret.
//
// Canales: Telegram (entrega inmediata, para operación) y correo vía Resend
// (registro formal). Ambos opcionales: si no hay credencial configurada, ese
// canal simplemente se omite.
// ============================================================================

import { createClient } from "jsr:@supabase/supabase-js@2";
import { CORS, igualdadSegura, json } from "../_shared/contrato.ts";

const db = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
  { auth: { persistSession: false, autoRefreshToken: false } },
);

const TELEGRAM_TOKEN = Deno.env.get("TELEGRAM_BOT_TOKEN") ?? "";
const TELEGRAM_CHAT = Deno.env.get("TELEGRAM_CHAT_ID") ?? "";
const RESEND_KEY = Deno.env.get("RESEND_API_KEY") ?? "";
const CORREO_DESTINO = Deno.env.get("CORREO_DESTINO") ?? "";
const CORREO_ORIGEN = Deno.env.get("CORREO_ORIGEN") ?? "alertas@example.com";
const URL_DASHBOARD = Deno.env.get("URL_DASHBOARD") ?? "";

const ICONO: Record<string, string> = { warning: "⚠️", alarm: "🚨" };

const DESCRIPCION_DIR: Record<string, string> = {
  high: "por encima del límite",
  low: "por debajo del límite",
  falla: "sin lectura válida (sensor desconectado o dañado)",
};

function formatearValor(v: number | null, unidad: string, decimales: number): string {
  return v === null ? "—" : `${v.toFixed(decimales)} ${unidad}`;
}

async function enviarTelegram(texto: string): Promise<void> {
  if (!TELEGRAM_TOKEN || !TELEGRAM_CHAT) return;
  const r = await fetch(`https://api.telegram.org/bot${TELEGRAM_TOKEN}/sendMessage`, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({
      chat_id: TELEGRAM_CHAT,
      text: texto,
      parse_mode: "HTML",
      disable_web_page_preview: true,
    }),
  });
  if (!r.ok) console.error("Telegram falló", r.status, await r.text());
}

async function enviarCorreo(asunto: string, html: string): Promise<void> {
  if (!RESEND_KEY || !CORREO_DESTINO) return;
  const r = await fetch("https://api.resend.com/emails", {
    method: "POST",
    headers: {
      Authorization: `Bearer ${RESEND_KEY}`,
      "Content-Type": "application/json",
    },
    body: JSON.stringify({
      from: CORREO_ORIGEN,
      to: CORREO_DESTINO.split(",").map((s) => s.trim()),
      subject: asunto,
      html,
    }),
  });
  if (!r.ok) console.error("Resend falló", r.status, await r.text());
}

Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response("ok", { headers: CORS });
  if (req.method !== "POST") return json({ error: "método no permitido" }, 405);

  // --- Autenticación del trigger --------------------------------------------
  const { data: cfg } = await db
    .from("app_config")
    .select("valor")
    .eq("clave", "notify_secret")
    .maybeSingle();

  const esperado = cfg?.valor ?? "";
  const recibido = req.headers.get("x-notify-secret") ?? "";
  if (!esperado || !igualdadSegura(recibido, esperado)) {
    return json({ error: "no autorizado" }, 401);
  }

  const { alert_id } = await req.json().catch(() => ({ alert_id: null }));
  if (!alert_id) return json({ error: "falta alert_id" }, 400);

  // --- Datos de la alarma ---------------------------------------------------
  const { data: a, error } = await db
    .from("alerts")
    .select(`
      id, nivel, direccion, valor_disparo, umbral, abierta_at,
      sensors ( slug, etiqueta, unidad, decimales ),
      devices ( slug, nombre, ubicacion )
    `)
    .eq("id", alert_id)
    .maybeSingle();

  if (error || !a) return json({ error: "alarma no encontrada" }, 404);

  const sensor = a.sensors as unknown as {
    etiqueta: string; unidad: string; decimales: number;
  };
  const equipo = a.devices as unknown as {
    nombre: string; ubicacion: string | null;
  };

  const icono = ICONO[a.nivel] ?? "•";
  const titulo = a.nivel === "alarm" ? "ALARMA" : "Advertencia";
  const valor = formatearValor(a.valor_disparo, sensor.unidad, sensor.decimales);
  const limite = a.umbral === null
    ? "—"
    : formatearValor(a.umbral, sensor.unidad, sensor.decimales);
  const cuando = new Date(a.abierta_at).toLocaleString("es-MX", {
    timeZone: "America/Mexico_City",
  });

  const lineas = [
    `${icono} <b>${titulo}</b> · ${sensor.etiqueta}`,
    ``,
    `Equipo: ${equipo.nombre}${equipo.ubicacion ? ` (${equipo.ubicacion})` : ""}`,
    `Condición: ${DESCRIPCION_DIR[a.direccion] ?? a.direccion}`,
    `Valor: <b>${valor}</b>${a.direccion !== "falla" ? `  ·  Límite: ${limite}` : ""}`,
    `Hora: ${cuando}`,
  ];
  if (URL_DASHBOARD) lineas.push(``, `Ver dashboard: ${URL_DASHBOARD}`);

  const html = `
    <div style="font-family:system-ui,-apple-system,sans-serif;max-width:520px">
      <h2 style="color:${a.nivel === "alarm" ? "#b91c1c" : "#b45309"};margin:0 0 12px">
        ${icono} ${titulo}: ${sensor.etiqueta}
      </h2>
      <table style="border-collapse:collapse;font-size:14px">
        <tr><td style="padding:4px 12px 4px 0;color:#666">Equipo</td><td><b>${equipo.nombre}</b></td></tr>
        <tr><td style="padding:4px 12px 4px 0;color:#666">Ubicación</td><td>${equipo.ubicacion ?? "—"}</td></tr>
        <tr><td style="padding:4px 12px 4px 0;color:#666">Condición</td><td>${DESCRIPCION_DIR[a.direccion] ?? a.direccion}</td></tr>
        <tr><td style="padding:4px 12px 4px 0;color:#666">Valor</td><td><b>${valor}</b></td></tr>
        <tr><td style="padding:4px 12px 4px 0;color:#666">Límite</td><td>${limite}</td></tr>
        <tr><td style="padding:4px 12px 4px 0;color:#666">Hora</td><td>${cuando}</td></tr>
      </table>
      ${URL_DASHBOARD ? `<p style="margin-top:16px"><a href="${URL_DASHBOARD}">Abrir dashboard</a></p>` : ""}
    </div>`;

  // Ambos canales en paralelo. allSettled y no all: que falle Telegram no debe
  // impedir el correo, ni al revés.
  const res = await Promise.allSettled([
    enviarTelegram(lineas.join("\n")),
    enviarCorreo(`${icono} ${titulo}: ${sensor.etiqueta} — ${equipo.nombre}`, html),
  ]);

  const fallos = res.filter((r) => r.status === "rejected").length;
  return json({ ok: true, canales_fallidos: fallos });
});
