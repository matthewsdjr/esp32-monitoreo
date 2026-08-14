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
  /** Versión breve para la insignia de la tarjeta, que es angosta. */
  etiquetaCorta: string;
  icono: string;
}

/**
 * Antigüedad a partir de la cual una lectura se considera obsoleta.
 *
 * DEPENDE DEL MODO DE ENLACE, y no tenerlo en cuenta era un error: con el canal
 * en vivo el equipo publica cada 2 s, así que 15 s son más de siete ciclos
 * perdidos y delatan un problema real. Pero cuando el navegador cae a consulta
 * periódica, el dato más fresco disponible es el del último LOTE, que llega
 * cada 30 s — con el umbral de 15 s todas las tarjetas se marcarían obsoletas
 * de forma permanente aunque el sistema funcione perfectamente.
 */
export const MS_OBSOLETO_VIVO = 15_000;

/** Tres lotes perdidos: eso sí indica que el equipo dejó de reportar. */
export const MS_OBSOLETO_DEGRADADO = 95_000;

export function msObsoletoSegun(enlace: string): number {
  return enlace === "vivo" || enlace === "demo"
    ? MS_OBSOLETO_VIVO
    : MS_OBSOLETO_DEGRADADO;
}

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
  msObsoleto: number = MS_OBSOLETO_VIVO,
): EvaluacionCanal {
  const canal = sensor.slug;
  const valor = valorDe(lectura, canal);

  // 1. Sin ningún dato todavía
  if (!lectura) {
    return { canal, valor: null, estado: "sin_dato", severidad: "normal",
             etiquetaEstado: "Sin dato", etiquetaCorta: "Sin dato", icono: "○" };
  }

  // 2. Falla reportada por el firmware (bit en la máscara)
  const enFalla = (lectura.faults & (1 << BIT_FALLA[canal])) !== 0;
  if (enFalla || valor === null) {
    return { canal, valor: null, estado: "falla", severidad: "alarma",
             etiquetaEstado: "Falla de sensor", etiquetaCorta: "Falla", icono: "✕" };
  }

  // 3. Dato viejo. Se evalúa DESPUÉS de la falla porque una falla explícita es
  //    información más precisa que la simple antigüedad.
  const edad = ahoraMs - Date.parse(lectura.ts);
  if (edad > msObsoleto) {
    return { canal, valor, estado: "obsoleto", severidad: "advertencia",
             etiquetaEstado: "Dato obsoleto", etiquetaCorta: "Obsoleto", icono: "◷" };
  }

  // 4. Fuera del rango físico del sensor: casi siempre cableado suelto o sensor
  //    dañado, no un valor real del proceso.
  const { min_fisico, max_fisico } = sensor;
  if ((min_fisico !== null && valor < min_fisico) || (max_fisico !== null && valor > max_fisico)) {
    return { canal, valor, estado: "fuera_rango", severidad: "alarma",
             etiquetaEstado: "Fuera de rango físico", etiquetaCorta: "Fuera de rango", icono: "✕" };
  }

  // 5. Umbrales de proceso
  if (umbral) {
    const { alarm_low, alarm_high, warn_low, warn_high } = umbral;
    if ((alarm_high !== null && valor > alarm_high) || (alarm_low !== null && valor < alarm_low)) {
      return { canal, valor, estado: "ok", severidad: "alarma",
               etiquetaEstado: "En alarma", etiquetaCorta: "Alarma", icono: "▲" };
    }
    if ((warn_high !== null && valor > warn_high) || (warn_low !== null && valor < warn_low)) {
      return { canal, valor, estado: "ok", severidad: "advertencia",
               etiquetaEstado: "Fuera de rango objetivo", etiquetaCorta: "Advertencia", icono: "▲" };
    }
  }

  return { canal, valor, estado: "ok", severidad: "normal",
           etiquetaEstado: "Normal", etiquetaCorta: "Normal", icono: "●" };
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
