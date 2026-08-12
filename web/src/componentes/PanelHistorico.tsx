// ============================================================================
// Panel histórico: filtro compartido + gráficas + vista de tabla
// ============================================================================
// El selector de rango vive ARRIBA de todo lo que afecta, no dentro de cada
// tarjeta: las tres gráficas se re-dibujan siempre contra la misma rebanada de
// tiempo, para que sean comparables entre sí.
// ============================================================================

import { useMemo, useState } from "react";
import type { Lectura, RangoHistorico, Umbral } from "../tipos";
import { RANGOS } from "../tipos";
import { GraficaSerie, type SerieDef } from "./GraficaSerie";
import { TablaDatos } from "./TablaDatos";
import { descargarCSV } from "../logica/exportar";

interface Props {
  datos: Lectura[];
  umbrales: Umbral[];
  rango: RangoHistorico;
  alCambiarRango: (r: RangoHistorico) => void;
  cargando: boolean;
}

// Los termopares comparten gráfica porque comparten unidad Y escala real.
// El ambiente (~22 °C) va aparte: junto a ellos quedaría pegado al piso del eje
// y comprimiría toda la variación del proceso.
const PROCESO: SerieDef[] = [
  { clave: "tc1_c", nombre: "Termopar 1", color: "var(--series-1)" },
  { clave: "tc2_c", nombre: "Termopar 2", color: "var(--series-2)" },
];

const PESO: SerieDef[] = [{ clave: "peso_g", nombre: "Peso", color: "var(--series-1)" }];
const HUMEDAD: SerieDef[] = [{ clave: "hum_pct", nombre: "Humedad", color: "var(--series-1)" }];
const AMBIENTE: SerieDef[] = [{ clave: "temp_amb_c", nombre: "Ambiente", color: "var(--series-1)" }];

export function PanelHistorico({ datos, umbrales, rango, alCambiarRango, cargando }: Props) {
  const [vista, setVista] = useState<"graficas" | "tabla">("graficas");

  const umbralPor = useMemo(
    () => Object.fromEntries(umbrales.map((u) => [u.canal, u])) as Record<string, Umbral>,
    [umbrales],
  );

  const rangoLargo = RANGOS[rango].ms > RANGOS["24h"].ms;

  return (
    <section className="flex flex-col gap-3">
      {/* Una sola fila de filtros para todo lo que sigue */}
      <div className="flex flex-wrap items-center gap-2">
        <div
          className="inline-flex rounded-lg overflow-hidden"
          style={{ border: "1px solid var(--border)" }}
          role="group"
          aria-label="Rango de tiempo"
        >
          {(Object.keys(RANGOS) as RangoHistorico[]).map((r) => (
            <button
              key={r}
              onClick={() => alCambiarRango(r)}
              aria-pressed={rango === r}
              className="px-3 py-1.5 text-xs font-medium transition-colors"
              style={{
                background: rango === r ? "var(--series-1)" : "transparent",
                color: rango === r ? "#fff" : "var(--text-secondary)",
              }}
            >
              {RANGOS[r].etiqueta}
            </button>
          ))}
        </div>

        <div
          className="inline-flex rounded-lg overflow-hidden"
          style={{ border: "1px solid var(--border)" }}
          role="group"
          aria-label="Modo de visualización"
        >
          {(["graficas", "tabla"] as const).map((v) => (
            <button
              key={v}
              onClick={() => setVista(v)}
              aria-pressed={vista === v}
              className="px-3 py-1.5 text-xs font-medium transition-colors"
              style={{
                background: vista === v ? "var(--series-1)" : "transparent",
                color: vista === v ? "#fff" : "var(--text-secondary)",
              }}
            >
              {v === "graficas" ? "Gráficas" : "Tabla"}
            </button>
          ))}
        </div>

        <button
          onClick={() => descargarCSV(datos, rango)}
          disabled={datos.length === 0}
          className="ml-auto px-3 py-1.5 text-xs font-medium rounded-lg transition-opacity
                     hover:opacity-70 disabled:opacity-40"
          style={{ border: "1px solid var(--border)", color: "var(--text-secondary)" }}
        >
          ↓ Exportar CSV
        </button>
      </div>

      {rangoLargo && (
        <p className="text-xs" style={{ color: "var(--text-muted)" }}>
          Mostrando promedios de 5 minutos. Los rangos de hasta 24 horas usan lecturas sin agregar.
        </p>
      )}

      {/* Al recargar se mantiene el render anterior atenuado, en vez de mostrar
          un esqueleto: evita el salto de layout y el parpadeo. */}
      <div
        className="flex flex-col gap-3 transition-opacity"
        style={{ opacity: cargando ? 0.55 : 1 }}
      >
        {vista === "tabla" ? (
          <TablaDatos datos={datos} />
        ) : (
          <>
            <GraficaSerie
              titulo="Temperaturas de proceso"
              unidad="°C"
              decimales={1}
              series={PROCESO}
              datos={datos}
              umbral={umbralPor["tc1"]}
              rangoLargo={rangoLargo}
            />
            <div className="grid gap-3 lg:grid-cols-3">
              <GraficaSerie
                titulo="Peso"
                unidad="g"
                decimales={0}
                series={PESO}
                datos={datos}
                rangoLargo={rangoLargo}
              />
              <GraficaSerie
                titulo="Temperatura ambiente"
                unidad="°C"
                decimales={1}
                series={AMBIENTE}
                datos={datos}
                umbral={umbralPor["temp_amb"]}
                rangoLargo={rangoLargo}
              />
              <GraficaSerie
                titulo="Humedad relativa"
                unidad="%HR"
                decimales={1}
                series={HUMEDAD}
                datos={datos}
                umbral={umbralPor["hum"]}
                rangoLargo={rangoLargo}
              />
            </div>
          </>
        )}
      </div>
    </section>
  );
}
