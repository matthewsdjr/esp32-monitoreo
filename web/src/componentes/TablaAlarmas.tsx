// ============================================================================
// Alarmas: activas destacadas arriba, historial abajo
// ============================================================================

import { useState } from "react";
import type { Alarma } from "../tipos";
import { haceCuanto } from "../logica/estado";

interface Props {
  alarmas: Alarma[];
  ahoraMs: number;
  alReconocer: (id: number, por: string) => void;
  soloLectura: boolean;
}

const FMT = new Intl.DateTimeFormat("es-MX", {
  day: "2-digit", month: "short", hour: "2-digit", minute: "2-digit",
});

const NIVEL = {
  alarm:   { texto: "Alarma",      color: "var(--status-critical)", icono: "🚨" },
  warning: { texto: "Advertencia", color: "var(--status-warning)",  icono: "⚠" },
} as const;

const DIRECCION = {
  high:  "por encima del límite",
  low:   "por debajo del límite",
  falla: "sin lectura válida",
} as const;

function duracion(a: Alarma, ahoraMs: number): string {
  const fin = a.cerrada_at ? Date.parse(a.cerrada_at) : ahoraMs;
  const min = Math.max(0, Math.round((fin - Date.parse(a.abierta_at)) / 60_000));
  if (min < 60) return `${min} min`;
  const h = Math.floor(min / 60);
  return `${h} h ${min % 60} min`;
}

export function TablaAlarmas({ alarmas, ahoraMs, alReconocer, soloLectura }: Props) {
  const [quien, setQuien] = useState("");
  const [editando, setEditando] = useState<number | null>(null);

  const activas = alarmas.filter((a) => !a.cerrada_at);
  const historial = alarmas.filter((a) => a.cerrada_at);

  return (
    <section
      className="rounded-xl p-4"
      style={{ background: "var(--surface-1)", border: "1px solid var(--border)" }}
    >
      <h3 className="text-sm font-semibold mb-3">Alarmas</h3>

      {activas.length === 0 ? (
        <p
          className="text-xs flex items-center gap-2 py-2"
          style={{ color: "var(--status-good)" }}
        >
          <span aria-hidden="true">●</span> Sin alarmas activas
        </p>
      ) : (
        <ul className="flex flex-col gap-2 mb-4">
          {activas.map((a) => {
            const n = NIVEL[a.nivel];
            return (
              <li
                key={a.id}
                className="rounded-lg p-3 flex flex-wrap items-center gap-x-4 gap-y-2"
                style={{
                  background: `color-mix(in srgb, ${n.color} 8%, transparent)`,
                  border: `1px solid color-mix(in srgb, ${n.color} 30%, transparent)`,
                }}
              >
                <span className="text-xs font-semibold flex items-center gap-1.5" style={{ color: n.color }}>
                  <span aria-hidden="true">{n.icono}</span> {n.texto}
                </span>

                <span className="text-sm font-medium">{a.etiqueta}</span>

                <span className="text-xs" style={{ color: "var(--text-secondary)" }}>
                  {DIRECCION[a.direccion]}
                  {a.valor_pico !== null && a.umbral !== null && (
                    <> · pico <b className="tabular-nums">{a.valor_pico.toFixed(1)}</b>
                       {" "}(límite {a.umbral.toFixed(1)})</>
                  )}
                </span>

                <span className="text-xs" style={{ color: "var(--text-muted)" }}>
                  {duracion(a, ahoraMs)} · desde {FMT.format(Date.parse(a.abierta_at))}
                </span>

                <div className="ml-auto">
                  {a.reconocida_por ? (
                    <span className="text-xs" style={{ color: "var(--text-muted)" }}>
                      ✓ {a.reconocida_por}
                    </span>
                  ) : soloLectura ? null : editando === a.id ? (
                    <form
                      className="flex items-center gap-1"
                      onSubmit={(e) => {
                        e.preventDefault();
                        if (quien.trim().length >= 2) {
                          alReconocer(a.id, quien.trim());
                          setEditando(null);
                          setQuien("");
                        }
                      }}
                    >
                      <input
                        autoFocus
                        value={quien}
                        onChange={(e) => setQuien(e.target.value)}
                        placeholder="Tu nombre"
                        className="text-xs rounded px-2 py-1 w-32"
                        style={{
                          border: "1px solid var(--border)",
                          background: "var(--surface-1)",
                          color: "var(--text-primary)",
                        }}
                      />
                      <button
                        type="submit"
                        className="text-xs rounded px-2 py-1 font-medium"
                        style={{ background: "var(--series-1)", color: "#fff" }}
                      >
                        OK
                      </button>
                    </form>
                  ) : (
                    <button
                      onClick={() => setEditando(a.id)}
                      className="text-xs rounded px-2.5 py-1 transition-opacity hover:opacity-70"
                      style={{ border: "1px solid var(--border)", color: "var(--text-secondary)" }}
                    >
                      Reconocer
                    </button>
                  )}
                </div>
              </li>
            );
          })}
        </ul>
      )}

      {historial.length > 0 && (
        <div className="overflow-x-auto">
          <table className="w-full text-xs border-collapse">
            <thead>
              <tr style={{ color: "var(--text-muted)" }}>
                <th className="text-left font-medium py-2 pr-4">Nivel</th>
                <th className="text-left font-medium py-2 pr-4">Sensor</th>
                <th className="text-left font-medium py-2 pr-4">Condición</th>
                <th className="text-right font-medium py-2 pr-4">Pico</th>
                <th className="text-left font-medium py-2 pr-4 whitespace-nowrap">Inicio</th>
                <th className="text-right font-medium py-2 pr-4">Duración</th>
                <th className="text-left font-medium py-2">Reconoció</th>
              </tr>
            </thead>
            <tbody>
              {historial.map((a) => {
                const n = NIVEL[a.nivel];
                return (
                  <tr key={a.id} style={{ borderTop: "1px solid var(--grid)" }}>
                    <td className="py-1.5 pr-4 whitespace-nowrap" style={{ color: n.color }}>
                      <span aria-hidden="true">{n.icono}</span> {n.texto}
                    </td>
                    <td className="py-1.5 pr-4">{a.etiqueta}</td>
                    <td className="py-1.5 pr-4" style={{ color: "var(--text-secondary)" }}>
                      {DIRECCION[a.direccion]}
                    </td>
                    <td className="py-1.5 pr-4 text-right tabular-nums">
                      {a.valor_pico?.toFixed(1) ?? "—"}
                    </td>
                    <td className="py-1.5 pr-4 whitespace-nowrap" style={{ color: "var(--text-secondary)" }}>
                      {FMT.format(Date.parse(a.abierta_at))}
                    </td>
                    <td className="py-1.5 pr-4 text-right tabular-nums">{duracion(a, ahoraMs)}</td>
                    <td className="py-1.5" style={{ color: "var(--text-muted)" }}>
                      {a.reconocida_por ?? "—"}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
      )}

      <p className="sr-only">
        {activas.length} alarmas activas, {historial.length} en el historial.
        Última actualización {haceCuanto(new Date(ahoraMs).toISOString(), ahoraMs)}.
      </p>
    </section>
  );
}
