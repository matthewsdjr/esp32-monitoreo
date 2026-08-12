// ============================================================================
// Derivación del estado de cada canal
// ============================================================================

import type {
  Canal,
  DefinicionSensor,
  EstadoCanal,
  Lectura,
  Umbral,
} from "../tipos";
import { BIT_FALLA, COLUMNA } from "../tipos";

/** Severidad de la lectura respecto a sus umbrales. */
export type Severidad = "normal" | "advertencia" | "alarma";

export interface EvaluacionCanal {
  canal: Canal;
  valor: number | null;
  estado: EstadoCanal;
  severidad: Severidad;
  /** Texto que acompaña siempre al color: el estado nunca se comunica solo con el tono. */
  etiquetaEstado: string;
  icono: string;
}

/**
 * Antigüedad a partir de la cual una lectura se considera obsoleta.
 * El equipo publica cada 2 s; 15 s son más de siete ciclos perdidos, lo que ya
 * no se explica por jitter de red.
 */
export const MS_OBSOLETO = 15_000;

export function valorDe(l: Lectura | null, canal: Canal): number | null {
  if (!l) return null;
  const v = (l as unknown as Record<string, number | null>)[COLUMNA[canal]];
  return typeof v === "number" && Number.isFinite(v) ? v : null;
}

export function evaluarCanal(
  sensor: DefinicionSensor,
  lectura: Lectura | null,
  umbral: Umbral | undefined,
  ahoraMs: number,
): EvaluacionCanal {
  const canal = sensor.slug;
  const valor = valorDe(lectura, canal);

  // 1. Sin ningún dato todavía
  if (!lectura) {
    return { canal, valor: null, estado: "sin_dato", severidad: "normal",
             etiquetaEstado: "Sin dato", icono: "○" };
  }

  // 2. Falla reportada por el firmware (bit en la máscara)
  const enFalla = (lectura.faults & (1 << BIT_FALLA[canal])) !== 0;
  if (enFalla || valor === null) {
    return { canal, valor: null, estado: "falla", severidad: "alarma",
             etiquetaEstado: "Falla de sensor", icono: "✕" };
  }

  // 3. Dato viejo. Se evalúa DESPUÉS de la falla porque una falla explícita es
  //    información más precisa que la simple antigüedad.
  const edad = ahoraMs - Date.parse(lectura.ts);
  if (edad > MS_OBSOLETO) {
    return { canal, valor, estado: "obsoleto", severidad: "advertencia",
             etiquetaEstado: "Dato obsoleto", icono: "◷" };
  }

  // 4. Fuera del rango físico del sensor: casi siempre cableado suelto o sensor
  //    dañado, no un valor real del proceso.
  const { min_fisico, max_fisico } = sensor;
  if ((min_fisico !== null && valor < min_fisico) || (max_fisico !== null && valor > max_fisico)) {
    return { canal, valor, estado: "fuera_rango", severidad: "alarma",
             etiquetaEstado: "Fuera de rango físico", icono: "✕" };
  }

  // 5. Umbrales de proceso
  if (umbral) {
    const { alarm_low, alarm_high, warn_low, warn_high } = umbral;
    if ((alarm_high !== null && valor > alarm_high) || (alarm_low !== null && valor < alarm_low)) {
      return { canal, valor, estado: "ok", severidad: "alarma",
               etiquetaEstado: "En alarma", icono: "▲" };
    }
    if ((warn_high !== null && valor > warn_high) || (warn_low !== null && valor < warn_low)) {
      return { canal, valor, estado: "ok", severidad: "advertencia",
               etiquetaEstado: "Fuera de rango objetivo", icono: "▲" };
    }
  }

  return { canal, valor, estado: "ok", severidad: "normal",
           etiquetaEstado: "Normal", icono: "●" };
}

/** Color de estado. Va SIEMPRE acompañado de icono y texto, nunca solo. */
export function colorSeveridad(sev: Severidad, estado: EstadoCanal): string {
  if (estado === "falla" || estado === "fuera_rango") return "var(--status-critical)";
  if (estado === "obsoleto") return "var(--status-serious)";
  if (estado === "sin_dato") return "var(--text-muted)";
  if (sev === "alarma") return "var(--status-critical)";
  if (sev === "advertencia") return "var(--status-warning)";
  return "var(--status-good)";
}

export function formatear(v: number | null, decimales: number): string {
  if (v === null) return "—";
  return v.toLocaleString("es-MX", {
    minimumFractionDigits: decimales,
    maximumFractionDigits: decimales,
  });
}

export function haceCuanto(iso: string | null, ahoraMs: number): string {
  if (!iso) return "nunca";
  const s = Math.max(0, Math.round((ahoraMs - Date.parse(iso)) / 1000));
  if (s < 60) return `hace ${s} s`;
  const m = Math.round(s / 60);
  if (m < 60) return `hace ${m} min`;
  const h = Math.round(m / 60);
  if (h < 24) return `hace ${h} h`;
  return `hace ${Math.round(h / 24)} d`;
}

export function formatearUptime(s: number | null): string {
  if (s === null) return "—";
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  return d > 0 ? `${d} d ${h} h` : h > 0 ? `${h} h ${m} min` : `${m} min`;
}
