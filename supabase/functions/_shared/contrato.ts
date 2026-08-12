// ============================================================================
// Contrato de datos compartido entre Edge Functions
// ============================================================================
// Este archivo es la fuente de verdad del payload. El firmware (config.h) y el
// simulador replican estas mismas estructuras. Ver docs/API.md.
// ============================================================================

/** Canales del equipo. El orden fija el bit que cada uno ocupa en `faults`. */
export const CANALES = ["peso", "temp_amb", "hum", "tc1", "tc2"] as const;
export type Canal = (typeof CANALES)[number];

/** Bit de `faults` por canal. DEBE coincidir con sensors.bit_falla y con el firmware. */
export const BIT_FALLA: Record<Canal, number> = {
  peso: 0,
  temp_amb: 1,
  hum: 2,
  tc1: 3,
  tc2: 4,
};

/** Una muestra instantánea de todos los canales. */
export interface Muestra {
  /** ISO-8601 UTC, reloj del dispositivo sincronizado por NTP. */
  ts: string;
  peso_g?: number | null;
  temp_amb_c?: number | null;
  hum_pct?: number | null;
  tc1_c?: number | null;
  tc2_c?: number | null;
  /** Máscara de bits: 1 = canal en falla. */
  faults?: number;
  extra?: Record<string, unknown> | null;
}

/** Telemetría del propio equipo (no de los sensores). */
export interface Salud {
  rssi?: number;
  uptime_s?: number;
  free_heap?: number;
  reconnects?: number;
  fw?: string;
}

/** Cuerpo de POST /ingest (Ruta B). */
export interface LoteIngesta {
  device: string;
  samples: Muestra[];
  health?: Salud;
}

// ----------------------------------------------------------------------------
// Límites
// ----------------------------------------------------------------------------
export const LIMITES = {
  /** Muestras por lote. A 5 s/muestra, 120 cubre 10 min de búfer por petición. */
  MAX_MUESTRAS_LOTE: 120,
  /** Tamaño máximo del cuerpo. Evita agotar memoria de la función. */
  MAX_BYTES_CUERPO: 256 * 1024,
  /** Peticiones por minuto y por equipo. El ESP32 normal hace 2. */
  MAX_PETICIONES_MIN: 10,
  /**
   * Tolerancia hacia el futuro. Un `ts` posterior a esto indica reloj del
   * equipo desincronizado; aceptarlo ensuciaría el histórico con datos que
   * "ya ocurrieron" antes de ocurrir.
   */
  DERIVA_FUTURA_MAX_S: 120,
  /**
   * Antigüedad máxima aceptada. Permite drenar un búfer offline de hasta 7
   * días, y rechaza marcas de tiempo absurdas de un equipo sin sincronizar NTP.
   */
  ANTIGUEDAD_MAX_DIAS: 7,
} as const;

// ----------------------------------------------------------------------------
// Validación
// ----------------------------------------------------------------------------

export class ErrorValidacion extends Error {
  constructor(public codigo: string, mensaje: string) {
    super(mensaje);
  }
}

const esNumFinito = (v: unknown): v is number =>
  typeof v === "number" && Number.isFinite(v);

/**
 * Normaliza y valida un valor numérico de sensor.
 * Devuelve `null` (y no lanza) ante NaN/Infinity/ausente: una lectura inválida
 * es un hecho normal del mundo físico —sensor desconectado— y debe registrarse
 * como null, no tumbar el lote entero.
 */
function num(v: unknown): number | null {
  return esNumFinito(v) ? v : null;
}

export function validarLote(cuerpo: unknown): LoteIngesta {
  if (typeof cuerpo !== "object" || cuerpo === null) {
    throw new ErrorValidacion("cuerpo_invalido", "El cuerpo debe ser un objeto JSON");
  }
  const c = cuerpo as Record<string, unknown>;

  if (typeof c.device !== "string" || !/^[a-z0-9][a-z0-9-]{1,38}[a-z0-9]$/.test(c.device)) {
    throw new ErrorValidacion("device_invalido", "Campo `device` ausente o con formato inválido");
  }

  if (!Array.isArray(c.samples) || c.samples.length === 0) {
    throw new ErrorValidacion("samples_vacio", "Campo `samples` debe ser un arreglo no vacío");
  }
  if (c.samples.length > LIMITES.MAX_MUESTRAS_LOTE) {
    throw new ErrorValidacion(
      "lote_muy_grande",
      `Máximo ${LIMITES.MAX_MUESTRAS_LOTE} muestras por lote, se recibieron ${c.samples.length}`,
    );
  }

  const ahora = Date.now();
  const limiteFuturo = ahora + LIMITES.DERIVA_FUTURA_MAX_S * 1000;
  const limitePasado = ahora - LIMITES.ANTIGUEDAD_MAX_DIAS * 86_400_000;

  const samples: Muestra[] = c.samples.map((s, i) => {
    if (typeof s !== "object" || s === null) {
      throw new ErrorValidacion("muestra_invalida", `samples[${i}] no es un objeto`);
    }
    const m = s as Record<string, unknown>;

    if (typeof m.ts !== "string") {
      throw new ErrorValidacion("ts_ausente", `samples[${i}].ts ausente`);
    }
    const t = Date.parse(m.ts);
    if (Number.isNaN(t)) {
      throw new ErrorValidacion("ts_invalido", `samples[${i}].ts no es ISO-8601 válido`);
    }
    if (t > limiteFuturo) {
      throw new ErrorValidacion(
        "ts_futuro",
        `samples[${i}].ts está en el futuro; revisar sincronización NTP del equipo`,
      );
    }
    if (t < limitePasado) {
      throw new ErrorValidacion(
        "ts_antiguo",
        `samples[${i}].ts es anterior a ${LIMITES.ANTIGUEDAD_MAX_DIAS} días`,
      );
    }

    const faults = esNumFinito(m.faults) ? Math.trunc(m.faults) : 0;
    if (faults < 0 || faults > 0x7fff) {
      throw new ErrorValidacion("faults_invalido", `samples[${i}].faults fuera de rango`);
    }

    return {
      ts: new Date(t).toISOString(),
      peso_g: num(m.peso_g),
      temp_amb_c: num(m.temp_amb_c),
      hum_pct: num(m.hum_pct),
      tc1_c: num(m.tc1_c),
      tc2_c: num(m.tc2_c),
      faults,
      extra: (typeof m.extra === "object" ? m.extra : null) as Record<string, unknown> | null,
    };
  });

  const h = (typeof c.health === "object" && c.health !== null ? c.health : {}) as Record<string, unknown>;
  const health: Salud = {
    rssi: num(h.rssi) ?? undefined,
    uptime_s: num(h.uptime_s) ?? undefined,
    free_heap: num(h.free_heap) ?? undefined,
    reconnects: num(h.reconnects) ?? undefined,
    fw: typeof h.fw === "string" ? h.fw.slice(0, 32) : undefined,
  };

  return { device: c.device, samples, health };
}

// ----------------------------------------------------------------------------
// Utilidades
// ----------------------------------------------------------------------------

/** SHA-256 en hexadecimal. Ver nota en la migración 0001 sobre por qué no un KDF. */
export async function sha256Hex(texto: string): Promise<string> {
  const buf = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(texto));
  return Array.from(new Uint8Array(buf))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

/**
 * Comparación en tiempo constante.
 * Un `===` sobre el hash del token filtraría información por el tiempo de
 * ejecución, permitiendo reconstruirlo byte a byte.
 */
export function igualdadSegura(a: string, b: string): boolean {
  if (a.length !== b.length) return false;
  let dif = 0;
  for (let i = 0; i < a.length; i++) dif |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return dif === 0;
}

export const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Headers": "authorization, x-client-info, apikey, content-type",
  "Access-Control-Allow-Methods": "POST, OPTIONS",
};

export function json(datos: unknown, status = 200): Response {
  return new Response(JSON.stringify(datos), {
    status,
    headers: { ...CORS, "Content-Type": "application/json" },
  });
}
