// ============================================================================
// Vista de tabla — el gemelo accesible de las gráficas
// ============================================================================
// No es un extra: en modo claro una de las series queda por debajo de 3:1 de
// contraste contra la superficie, y la regla de relieve exige que todo valor sea
// alcanzable sin depender del color. Además es lo que la gente usa para copiar
// un dato puntual a un reporte.
// ============================================================================

import { useMemo } from "react";
import type { Lectura } from "../tipos";

const FMT = new Intl.DateTimeFormat("es-MX", {
  day: "2-digit", month: "2-digit", hour: "2-digit", minute: "2-digit", second: "2-digit",
});

const COLS: Array<{ clave: keyof Lectura; titulo: string; dec: number }> = [
  { clave: "peso_g",     titulo: "Peso (g)",      dec: 1 },
  { clave: "temp_amb_c", titulo: "Ambiente (°C)", dec: 1 },
  { clave: "hum_pct",    titulo: "Humedad (%HR)", dec: 1 },
  { clave: "tc1_c",      titulo: "Termopar 1 (°C)", dec: 1 },
  { clave: "tc2_c",      titulo: "Termopar 2 (°C)", dec: 1 },
];

const MAX_FILAS = 500;

export function TablaDatos({ datos }: { datos: Lectura[] }) {
  // Se muestran las más recientes primero y se acota el número de filas: pintar
  // 20 000 nodos bloquea el hilo principal en un celular.
  const filas = useMemo(() => [...datos].reverse().slice(0, MAX_FILAS), [datos]);

  return (
    <section
      className="rounded-xl p-4"
      style={{ background: "var(--surface-1)", border: "1px solid var(--border)" }}
    >
      <div className="flex items-baseline justify-between mb-3">
        <h3 className="text-sm font-semibold">Lecturas</h3>
        <span className="text-xs" style={{ color: "var(--text-muted)" }}>
          {datos.length > MAX_FILAS
            ? `${MAX_FILAS} más recientes de ${datos.length.toLocaleString("es-MX")} · exporta a CSV para el conjunto completo`
            : `${datos.length.toLocaleString("es-MX")} registros`}
        </span>
      </div>

      {/* El contenido ancho hace scroll dentro de su contenedor; la página
          nunca hace scroll horizontal. */}
      <div className="overflow-x-auto" style={{ maxHeight: 420 }}>
        <table className="w-full text-xs tabular-nums border-collapse">
          <thead className="sticky top-0" style={{ background: "var(--surface-1)" }}>
            <tr style={{ color: "var(--text-muted)" }}>
              <th className="text-left font-medium py-2 pr-4 whitespace-nowrap">Hora</th>
              {COLS.map((c) => (
                <th key={String(c.clave)} className="text-right font-medium py-2 pl-4 whitespace-nowrap">
                  {c.titulo}
                </th>
              ))}
              <th className="text-right font-medium py-2 pl-4">Estado</th>
            </tr>
          </thead>
          <tbody>
            {filas.map((f) => (
              <tr key={f.ts} style={{ borderTop: "1px solid var(--grid)" }}>
                <td className="py-1.5 pr-4 whitespace-nowrap" style={{ color: "var(--text-secondary)" }}>
                  {FMT.format(Date.parse(f.ts))}
                </td>
                {COLS.map((c) => {
                  const v = f[c.clave];
                  return (
                    <td key={String(c.clave)} className="text-right py-1.5 pl-4">
                      {typeof v === "number"
                        ? v.toFixed(c.dec)
                        : <span style={{ color: "var(--text-muted)" }}>—</span>}
                    </td>
                  );
                })}
                <td className="text-right py-1.5 pl-4">
                  {f.faults === 0
                    ? <span style={{ color: "var(--text-muted)" }}>OK</span>
                    : <span style={{ color: "var(--status-critical)" }}>✕ falla</span>}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}
