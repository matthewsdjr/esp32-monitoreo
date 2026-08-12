import type { Lectura, RangoHistorico } from "../tipos";

const COLUMNAS = [
  "ts", "peso_g", "temp_amb_c", "hum_pct", "tc1_c", "tc2_c", "faults",
] as const;

/**
 * Exporta el rango visible a CSV.
 *
 * Se usa punto decimal y coma como separador (RFC 4180). Excel en configuración
 * regional española espera punto y coma; se incluye la línea `sep=,` para que
 * abra correcto sin que el usuario tenga que usar el asistente de importación.
 */
export function descargarCSV(datos: Lectura[], rango: RangoHistorico): void {
  if (datos.length === 0) return;

  const lineas = [
    "sep=,",
    COLUMNAS.join(","),
    ...datos.map((d) =>
      COLUMNAS.map((c) => {
        const v = (d as unknown as Record<string, unknown>)[c];
        return v === null || v === undefined ? "" : String(v);
      }).join(","),
    ),
  ];

  const blob = new Blob([lineas.join("\n")], { type: "text/csv;charset=utf-8;" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  const sello = new Date().toISOString().slice(0, 19).replace(/[:T]/g, "-");
  a.href = url;
  a.download = `monitoreo-${rango}-${sello}.csv`;
  a.click();
  URL.revokeObjectURL(url);
}
