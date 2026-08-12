// ============================================================================
// Tipos del dominio — reflejan docs/API.md
// ============================================================================

export const CANALES = ["peso", "temp_amb", "hum", "tc1", "tc2"] as const;
export type Canal = (typeof CANALES)[number];

/** Bit que cada canal ocupa en `faults`. Contrato compartido con el firmware. */
export const BIT_FALLA: Record<Canal, number> = {
  peso: 0,
  temp_amb: 1,
  hum: 2,
  tc1: 3,
  tc2: 4,
};

/** Columna de `readings` que corresponde a cada canal. */
export const COLUMNA: Record<Canal, string> = {
  peso: "peso_g",
  temp_amb: "temp_amb_c",
  hum: "hum_pct",
  tc1: "tc1_c",
  tc2: "tc2_c",
};

export interface DefinicionSensor {
  slug: Canal;
  etiqueta: string;
  unidad: string;
  decimales: number;
  min_fisico: number | null;
  max_fisico: number | null;
}

export interface Lectura {
  ts: string;
  peso_g: number | null;
  temp_amb_c: number | null;
  hum_pct: number | null;
  tc1_c: number | null;
  tc2_c: number | null;
  faults: number;
}

/** Estado por canal. El orden importa: define la severidad. */
export type EstadoCanal = "ok" | "obsoleto" | "fuera_rango" | "falla" | "sin_dato";

export interface EstadoEquipo {
  slug: string;
  nombre: string;
  ubicacion: string | null;
  fw_version: string | null;
  last_seen_at: string | null;
  rssi: number | null;
  uptime_s: number | null;
  free_heap: number | null;
  reconnects: number | null;
  estado_conexion: "online" | "intermitente" | "offline" | "nunca_visto";
}

export interface Alarma {
  id: number;
  canal: Canal;
  etiqueta: string;
  nivel: "warning" | "alarm";
  direccion: "low" | "high" | "falla";
  valor_disparo: number | null;
  valor_pico: number | null;
  umbral: number | null;
  abierta_at: string;
  cerrada_at: string | null;
  reconocida_por: string | null;
}

export interface Umbral {
  canal: Canal;
  warn_low: number | null;
  warn_high: number | null;
  alarm_low: number | null;
  alarm_high: number | null;
}

/** Estado de la conexión del NAVEGADOR con el backend. */
export type EstadoEnlace = "vivo" | "degradado" | "desconectado" | "demo";

export type RangoHistorico = "1h" | "6h" | "24h" | "7d" | "30d";

export const RANGOS: Record<RangoHistorico, { etiqueta: string; ms: number }> = {
  "1h":  { etiqueta: "1 hora",   ms: 3_600_000 },
  "6h":  { etiqueta: "6 horas",  ms: 21_600_000 },
  "24h": { etiqueta: "24 horas", ms: 86_400_000 },
  "7d":  { etiqueta: "7 días",   ms: 604_800_000 },
  "30d": { etiqueta: "30 días",  ms: 2_592_000_000 },
};

/**
 * Por encima de 24 h el dashboard consulta los agregados de 5 min.
 * Consultar el crudo para un mes descargaría cientos de miles de puntos.
 */
export const UMBRAL_AGREGADO_MS = RANGOS["24h"].ms;
