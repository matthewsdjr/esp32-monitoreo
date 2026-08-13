// ============================================================================
// Edge Function · POST /ingest  —  Ruta B (persistencia)
// ============================================================================
// Único punto de escritura del sistema. Corre con service_role (omite RLS), por
// lo que TODA la autorización ocurre aquí explícitamente.
//
// El ESP32 llama a esta función cada 30 s con un lote de muestras acumuladas.
// Presupuesto: 2 880 invocaciones/día ≈ 86 400/mes, sobre un límite de 500 000.
//
// El tiempo real NO pasa por aquí: va por el canal broadcast de Realtime, que
// no consume invocaciones. Ver docs/ARQUITECTURA.md §3.1.
// ============================================================================

import { createClient } from "jsr:@supabase/supabase-js@2";
import {
  CORS,
  ErrorValidacion,
  igualdadSegura,
  json,
  LIMITES,
  sha256Hex,
  validarLote,
} from "../_shared/contrato.ts";

const URL_SUPABASE = Deno.env.get("SUPABASE_URL")!;
const CLAVE_SERVICIO = Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!;

const db = createClient(URL_SUPABASE, CLAVE_SERVICIO, {
  auth: { persistSession: false, autoRefreshToken: false },
});

// ----------------------------------------------------------------------------
// Rate limiting en memoria
// ----------------------------------------------------------------------------
// Deliberadamente en memoria y no en base: el objetivo es frenar a un equipo mal
// configurado que entre en bucle, no resistir un ataque distribuido. La defensa
// contra un tercero es el token, que no tiene. Un contador en base añadiría dos
// consultas a cada petición legítima para proteger contra un escenario que el
// token ya cubre.
const ventanas = new Map<string, { desde: number; n: number }>();

function excedeCuota(clave: string): boolean {
  const ahora = Date.now();
  const v = ventanas.get(clave);
  if (!v || ahora - v.desde > 60_000) {
    ventanas.set(clave, { desde: ahora, n: 1 });
    return false;
  }
  v.n++;
  return v.n > LIMITES.MAX_PETICIONES_MIN;
}

// ----------------------------------------------------------------------------
// Caché de equipos
// ----------------------------------------------------------------------------
// Evita ir a la base por el hash del token en cada petición. TTL corto para que
// una revocación de token surta efecto en menos de un minuto.
const TTL_CACHE_MS = 60_000;
const cacheEquipos = new Map<string, { id: string; token_hash: string; en: number }>();

async function resolverEquipo(slug: string) {
  const c = cacheEquipos.get(slug);
  if (c && Date.now() - c.en < TTL_CACHE_MS) return c;

  const { data, error } = await db
    .from("devices")
    .select("id, token_hash, activo")
    .eq("slug", slug)
    .maybeSingle();

  if (error) throw new Error(`consulta de equipo falló: ${error.message}`);
  if (!data || !data.activo) return null;

  const entrada = { id: data.id as string, token_hash: data.token_hash as string, en: Date.now() };
  cacheEquipos.set(slug, entrada);
  return entrada;
}

// ----------------------------------------------------------------------------
Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response("ok", { headers: CORS });
  if (req.method !== "POST") return json({ error: "método no permitido" }, 405);

  try {
    // --- 1. Token -----------------------------------------------------------
    const auth = req.headers.get("authorization") ?? "";
    const token = auth.startsWith("Bearer ") ? auth.slice(7).trim() : "";
    if (!token) return json({ error: "falta cabecera Authorization: Bearer <token>" }, 401);

    // --- 2. Tamaño ----------------------------------------------------------
    const largo = Number(req.headers.get("content-length") ?? "0");
    if (largo > LIMITES.MAX_BYTES_CUERPO) {
      return json({ error: "cuerpo demasiado grande" }, 413);
    }

    const crudo = await req.text();
    if (crudo.length > LIMITES.MAX_BYTES_CUERPO) {
      return json({ error: "cuerpo demasiado grande" }, 413);
    }

    // --- 3. Validación del payload -----------------------------------------
    let lote;
    try {
      lote = validarLote(JSON.parse(crudo));
    } catch (e) {
      if (e instanceof ErrorValidacion) return json({ error: e.message, codigo: e.codigo }, 400);
      return json({ error: "JSON malformado" }, 400);
    }

    // --- 4. Cuota -----------------------------------------------------------
    if (excedeCuota(lote.device)) {
      return json({ error: "demasiadas peticiones" }, 429);
    }

    // --- 5. Autorización ----------------------------------------------------
    const equipo = await resolverEquipo(lote.device);

    // Se calcula el hash SIEMPRE, incluso si el equipo no existe, para que el
    // tiempo de respuesta no revele qué slugs están dados de alta.
    const hash = await sha256Hex(token);
    if (!equipo || !igualdadSegura(hash, equipo.token_hash)) {
      return json({ error: "credenciales inválidas" }, 401);
    }

    // --- 6. Inserción de lecturas ------------------------------------------
    const filas = lote.samples.map((m) => ({
      device_id: equipo.id,
      ts: m.ts,
      peso_g: m.peso_g,
      temp_amb_c: m.temp_amb_c,
      hum_pct: m.hum_pct,
      tc1_c: m.tc1_c,
      tc2_c: m.tc2_c,
      faults: m.faults ?? 0,
      extra: m.extra,
    }));

    // ignoreDuplicates: si el ESP32 reintenta tras un timeout ambiguo, el lote
    // repetido no duplica filas ni falla. Es lo que permite que el drenado del
    // búfer offline sea agresivo sin ensuciar el histórico.
    const { error: errIns } = await db
      .from("readings")
      .upsert(filas, { onConflict: "device_id,ts", ignoreDuplicates: true });

    if (errIns) {
      console.error("inserción falló", errIns);
      return json({ error: "no se pudieron guardar las lecturas" }, 500);
    }

    // --- 6b. Resultados de comandos que el equipo ya ejecutó ---------------
    // El firmware adjunta aquí lo que hizo con los comandos que recibió en la
    // respuesta anterior. Cerrarlos en la misma petición evita una llamada
    // extra y mantiene el presupuesto de invocaciones intacto.
    if (Array.isArray(lote.resultados) && lote.resultados.length > 0) {
      await Promise.all(
        lote.resultados.slice(0, 20).map((r) =>
          db.from("device_commands")
            .update({
              estado: r.ok ? "ejecutado" : "fallido",
              ejecutado_at: new Date().toISOString(),
              resultado: r.detalle ?? null,
            })
            .eq("id", r.id)
            .eq("device_id", equipo.id) // un equipo no puede cerrar comandos de otro
            .in("estado", ["pendiente", "entregado"])
        ),
      );
    }

    // --- 7. Última lectura y salud del equipo ------------------------------
    const ultima = lote.samples.reduce((a, b) => (a.ts >= b.ts ? a : b));

    // Se ejecutan en paralelo: son independientes y ninguna condiciona a la otra.
    const [rLatest, rDev] = await Promise.all([
      db.from("latest_readings").upsert(
        {
          device_id: equipo.id,
          ts: ultima.ts,
          received_at: new Date().toISOString(),
          payload: ultima,
        },
        { onConflict: "device_id" },
      ),
      db
        .from("devices")
        .update({
          last_seen_at: new Date().toISOString(),
          fw_version: lote.health?.fw ?? undefined,
          rssi: lote.health?.rssi ?? undefined,
          uptime_s: lote.health?.uptime_s ?? undefined,
          free_heap: lote.health?.free_heap ?? undefined,
          reconnects: lote.health?.reconnects ?? undefined,
        })
        .eq("id", equipo.id),
    ]);

    // Estas dos fallas no invalidan la ingesta: las lecturas ya están a salvo.
    // Se registran, pero se responde 200 para que el ESP32 libere su búfer.
    if (rLatest.error) console.error("upsert latest_readings", rLatest.error);
    if (rDev.error) console.error("update devices", rDev.error);

    // --- 8. Comandos pendientes --------------------------------------------
    // Aquí es donde el sistema cruza el NAT en sentido inverso: el equipo no
    // acepta conexiones entrantes, así que los comandos viajan de vuelta en la
    // respuesta a su propia petición.
    const { data: pendientes } = await db
      .from("device_commands")
      .select("id, comando, parametros")
      .eq("device_id", equipo.id)
      .eq("estado", "pendiente")
      .gt("expira_at", new Date().toISOString())
      .order("solicitado_at")
      .limit(5);

    if (pendientes && pendientes.length > 0) {
      // Se marcan como entregados de inmediato. Si el equipo se reinicia antes
      // de ejecutarlos se pierden, y eso es lo correcto: reintentar una tara a
      // ciegas sobre una báscula cargada haría más daño que no hacer nada.
      await db.from("device_commands")
        .update({ estado: "entregado", entregado_at: new Date().toISOString() })
        .in("id", pendientes.map((c) => c.id));
    }

    return json({
      ok: true,
      recibidas: filas.length,
      ultima_ts: ultima.ts,
      servidor_ts: new Date().toISOString(),
      comandos: pendientes ?? [],
    });
  } catch (e) {
    console.error("error no controlado", e);
    return json({ error: "error interno" }, 500);
  }
});
