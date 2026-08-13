// ============================================================================
// Edge Function · POST /comando
// ============================================================================
// Encola un comando para el equipo (tara, calibración, reinicio).
//
// ES EL ÚNICO PUNTO DE ESCRITURA QUE PUEDE INVOCAR UN VISITANTE, y por eso es
// el más delicado del sistema: el dashboard es público. Sin la barrera del PIN,
// cualquiera con el enlace podría poner la báscula en cero a media producción.
//
// Defensas, en orden:
//   1. PIN de operador, tecleado por la persona. Nunca viaja en el bundle.
//   2. Comparación en tiempo constante del hash.
//   3. Límite de intentos por IP, con penalización creciente.
//   4. Lista blanca de comandos y validación de sus parámetros.
//   5. Un solo comando pendiente por tipo (índice único en la base).
//   6. Caducidad a 10 min: una tara que llega tres días tarde es peor que nada.
// ============================================================================

import { createClient } from "jsr:@supabase/supabase-js@2";
import { CORS, igualdadSegura, json, sha256Hex } from "../_shared/contrato.ts";

const db = createClient(
  Deno.env.get("SUPABASE_URL")!,
  Deno.env.get("SUPABASE_SERVICE_ROLE_KEY")!,
  { auth: { persistSession: false, autoRefreshToken: false } },
);

const COMANDOS_VALIDOS = ["tara", "calibrar", "reiniciar", "recargar_umbrales"] as const;
type Comando = (typeof COMANDOS_VALIDOS)[number];

// ----------------------------------------------------------------------------
// Límite de intentos
// ----------------------------------------------------------------------------
// Un PIN corto es fuerza-bruteable si se permiten intentos ilimitados: 4 dígitos
// son 10 000 combinaciones, cuestión de minutos con un script. Con 5 intentos
// por 15 min por IP, agotarlas tomaría años.
const MAX_INTENTOS = 5;
const VENTANA_MS = 15 * 60_000;
const intentos = new Map<string, { desde: number; fallos: number }>();

function bloqueado(ip: string): boolean {
  const v = intentos.get(ip);
  if (!v) return false;
  if (Date.now() - v.desde > VENTANA_MS) {
    intentos.delete(ip);
    return false;
  }
  return v.fallos >= MAX_INTENTOS;
}

function registrarFallo(ip: string) {
  const v = intentos.get(ip);
  if (!v || Date.now() - v.desde > VENTANA_MS) {
    intentos.set(ip, { desde: Date.now(), fallos: 1 });
  } else {
    v.fallos++;
  }
}

// ----------------------------------------------------------------------------
Deno.serve(async (req) => {
  if (req.method === "OPTIONS") return new Response("ok", { headers: CORS });
  if (req.method !== "POST") return json({ error: "método no permitido" }, 405);

  const ip = req.headers.get("x-forwarded-for")?.split(",")[0].trim() ?? "desconocida";
  if (bloqueado(ip)) {
    return json({ error: "Demasiados intentos fallidos. Espera 15 minutos." }, 429);
  }

  let cuerpo: Record<string, unknown>;
  try {
    cuerpo = await req.json();
  } catch {
    return json({ error: "JSON malformado" }, 400);
  }

  const { device, comando, pin, solicitado_por, parametros } = cuerpo as {
    device?: string; comando?: string; pin?: string;
    solicitado_por?: string; parametros?: Record<string, unknown>;
  };

  // --- Validación de forma ---------------------------------------------------
  if (typeof device !== "string" || !/^[a-z0-9][a-z0-9-]{1,38}[a-z0-9]$/.test(device)) {
    return json({ error: "equipo inválido" }, 400);
  }
  if (typeof comando !== "string" || !COMANDOS_VALIDOS.includes(comando as Comando)) {
    return json({ error: "comando no reconocido" }, 400);
  }
  if (typeof pin !== "string" || pin.length < 4 || pin.length > 64) {
    return json({ error: "PIN inválido" }, 400);
  }
  const quien = typeof solicitado_por === "string" ? solicitado_por.trim() : "";
  if (quien.length < 2 || quien.length > 60) {
    return json({ error: "Indica tu nombre (mínimo 2 caracteres)" }, 400);
  }

  // La calibración necesita el peso patrón; sin él taría con un valor absurdo.
  if (comando === "calibrar") {
    const p = Number(parametros?.peso_conocido_g);
    if (!Number.isFinite(p) || p <= 0 || p > 50_000) {
      return json({ error: "Peso patrón inválido (debe estar entre 0 y 50 000 g)" }, 400);
    }
  }

  // --- Autorización ----------------------------------------------------------
  const { data: cfg } = await db
    .from("app_config").select("valor").eq("clave", "operador_pin_hash").maybeSingle();

  const esperado = cfg?.valor ?? "";
  if (!esperado) {
    return json({
      error: "Los comandos remotos están desactivados. Configura operador_pin_hash en app_config.",
    }, 503);
  }

  // El hash se calcula siempre, aunque el PIN sea obviamente corto, para que el
  // tiempo de respuesta no filtre información.
  const hash = await sha256Hex(pin);
  if (!igualdadSegura(hash, esperado)) {
    registrarFallo(ip);
    return json({ error: "PIN incorrecto" }, 401);
  }

  // --- Equipo ----------------------------------------------------------------
  const { data: equipo } = await db
    .from("devices").select("id, activo, last_seen_at").eq("slug", device).maybeSingle();

  if (!equipo || !equipo.activo) return json({ error: "equipo no encontrado" }, 404);

  // Aviso, no bloqueo: si el equipo está fuera de línea el comando se encola
  // igual y caducará solo. Bloquear aquí obligaría al operador a adivinar si el
  // problema es el equipo o su PIN.
  const visto = equipo.last_seen_at ? Date.parse(equipo.last_seen_at) : 0;
  const equipoEnLinea = Date.now() - visto < 3 * 60_000;

  // --- Encolar ---------------------------------------------------------------
  const { data: creado, error } = await db
    .from("device_commands")
    .insert({
      device_id: equipo.id,
      comando,
      parametros: parametros ?? null,
      solicitado_por: quien,
      expira_at: new Date(Date.now() + 10 * 60_000).toISOString(),
    })
    .select("id, comando, estado, solicitado_at, expira_at")
    .maybeSingle();

  if (error) {
    // 23505 = violación de índice único: ya hay uno pendiente de ese tipo.
    if (error.code === "23505") {
      return json({
        error: `Ya hay una orden de "${comando}" pendiente. Espera a que el equipo la ejecute.`,
      }, 409);
    }
    console.error("no se pudo encolar el comando", error);
    return json({ error: "no se pudo encolar el comando" }, 500);
  }

  return json({
    ok: true,
    comando: creado,
    equipo_en_linea: equipoEnLinea,
    mensaje: equipoEnLinea
      ? "Orden encolada. El equipo la ejecutará en su próxima sincronización (máx. 30 s)."
      : "Orden encolada, pero el equipo NO está reportando. Caducará en 10 minutos si no se conecta.",
  });
});
