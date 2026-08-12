// ============================================================================
// Gráfica de serie temporal
// ============================================================================
// REGLA DE DISEÑO: una sola escala Y por gráfica, nunca eje dual.
// Superponer peso (0–50 000 g) y temperatura (0–1024 °C) con dos escalas
// inventa una correlación visual que no está en los datos, porque la alineación
// entre ambas escalas es arbitraria.
//
// Corolario menos obvio: compartir unidad NO basta para compartir gráfica. El
// ambiente (~22 °C) y los termopares (~120–220 °C) son ambos °C, pero juntos en
// un mismo eje el ambiente queda pegado al piso y las variaciones de los
// termopares se comprimen. Van en gráficas separadas — múltiplos pequeños.
// ============================================================================

import {
  CartesianGrid,
  Line,
  LineChart,
  ReferenceArea,
  ReferenceLine,
  ResponsiveContainer,
  Tooltip,
  XAxis,
  YAxis,
} from "recharts";
import type { Lectura, Umbral } from "../tipos";

export interface SerieDef {
  clave: keyof Lectura;
  nombre: string;
  color: string;
}

interface Props {
  titulo: string;
  unidad: string;
  decimales: number;
  series: SerieDef[];
  datos: Lectura[];
  umbral?: Umbral;
  rangoLargo: boolean;
}

const FMT_HORA = new Intl.DateTimeFormat("es-MX", { hour: "2-digit", minute: "2-digit" });
const FMT_FECHA = new Intl.DateTimeFormat("es-MX", { day: "2-digit", month: "short" });

interface EscalaY {
  dominio: [number, number];
  /** Marcas explícitas: dejar que Recharts las derive produce valores como 115 o 245. */
  marcas: number[];
  decimales: number;
}

/**
 * Dominio del eje Y a partir de los datos, no desde cero.
 *
 * Un umbral se incorpora SOLO si está razonablemente cerca de los datos. Sin
 * este filtro, una alarma de humedad baja en 25 %HR con el proceso operando a
 * 55 % estiraría el eje hasta el piso y aplastaría toda la variación real en
 * una franja de tres píxeles.
 */
function calcularDominio(
  datos: Lectura[],
  series: SerieDef[],
  umbral: Umbral | undefined,
): EscalaY | undefined {
  const vals: number[] = [];
  for (const d of datos) {
    for (const s of series) {
      const v = d[s.clave];
      if (typeof v === "number" && Number.isFinite(v)) vals.push(v);
    }
  }
  if (vals.length === 0) return undefined;

  let min = Math.min(...vals);
  let max = Math.max(...vals);
  const rango = max - min || Math.abs(max) * 0.1 || 1;

  if (umbral) {
    const cercanos = [umbral.warn_high, umbral.alarm_high, umbral.warn_low, umbral.alarm_low]
      .filter((u): u is number => u !== null)
      .filter((u) => u >= min - rango * 0.6 && u <= max + rango * 0.6);
    for (const u of cercanos) {
      min = Math.min(min, u);
      max = Math.max(max, u);
    }
  }

  const holgura = (max - min || 1) * 0.12;
  return escalaBonita(min - holgura, max + holgura);
}

/**
 * Convierte un intervalo arbitrario en una escala legible.
 * Sin esto el eje queda rotulado con 267 · 245 · 180 · 115 · 50.0 — valores
 * exactos que nadie lee de un vistazo. Con esto: 300 · 250 · 200 · 150 · 100.
 */
function escalaBonita(min: number, max: number): EscalaY {
  const rango = max - min || 1;

  const magnitud = Math.pow(10, Math.floor(Math.log10(rango / 4)));
  const norm = rango / 4 / magnitud;
  const paso = (norm <= 1 ? 1 : norm <= 2 ? 2 : norm <= 5 ? 5 : 10) * magnitud;

  const desde = Math.floor(min / paso) * paso;
  const hasta = Math.ceil(max / paso) * paso;

  const marcas: number[] = [];
  // La tolerancia evita que el error de punto flotante se coma la última marca.
  for (let v = desde; v <= hasta + paso * 1e-6; v += paso) {
    marcas.push(+v.toFixed(10));
  }

  return {
    dominio: [desde, hasta],
    marcas,
    decimales: paso >= 1 ? 0 : Math.min(3, Math.ceil(-Math.log10(paso))),
  };
}

export function GraficaSerie({
  titulo, unidad, decimales, series, datos, umbral, rangoLargo,
}: Props) {
  const puntos = datos.map((d) => ({ ...d, t: Date.parse(d.ts) }));
  const escala = calcularDominio(datos, series, umbral);
  const dominio = escala?.dominio;
  const ultimo = puntos[puntos.length - 1];

  const formatearEjeX = (v: number) =>
    rangoLargo ? FMT_FECHA.format(v) : FMT_HORA.format(v);

  const valorActual = (s: SerieDef): string => {
    const v = ultimo?.[s.clave];
    return typeof v === "number" ? `${v.toFixed(decimales)} ${unidad}` : "—";
  };

  return (
    <section
      className="rounded-xl p-4"
      style={{ background: "var(--surface-1)", border: "1px solid var(--border)" }}
    >
      <div className="flex items-baseline justify-between gap-3 mb-2">
        <h3 className="text-sm font-semibold">{titulo}</h3>
        {series.length === 1 ? (
          <span className="text-xs tabular-nums" style={{ color: "var(--text-secondary)" }}>
            {valorActual(series[0])}
          </span>
        ) : (
          <span className="text-xs" style={{ color: "var(--text-muted)" }}>{unidad}</span>
        )}
      </div>

      {/* Leyenda propia, arriba del plot y con el valor actual de cada serie.
          Sustituye a las etiquetas al final de cada línea, que se encimaban
          cuando dos series terminaban con valores cercanos. La identidad no
          queda a cargo del color solo: hay nombre, valor y vista de tabla. */}
      {series.length > 1 && (
        <ul className="flex flex-wrap gap-x-4 gap-y-1 mb-2">
          {series.map((s) => (
            <li key={String(s.clave)} className="flex items-center gap-1.5 text-xs">
              <span
                aria-hidden="true"
                className="inline-block rounded-full shrink-0"
                style={{ width: 12, height: 2.5, background: s.color }}
              />
              <span style={{ color: "var(--text-secondary)" }}>{s.nombre}</span>
              <span className="tabular-nums font-medium" style={{ color: "var(--text-primary)" }}>
                {valorActual(s)}
              </span>
            </li>
          ))}
        </ul>
      )}

      {/* Altura fija que INCLUYE la banda del eje X, para que la tarjeta no
          genere un scroll vertical interno. */}
      <div style={{ height: 236 }}>
        <ResponsiveContainer width="100%" height="100%">
          <LineChart data={puntos} margin={{ top: 12, right: 74, bottom: 4, left: 4 }}>
            {umbral?.alarm_high != null && (
              <ReferenceArea
                y1={umbral.alarm_high}
                y2={dominio ? Math.max(dominio[1], umbral.alarm_high) : umbral.alarm_high}
                fill="var(--status-critical)"
                fillOpacity={0.07}
                ifOverflow="hidden"
              />
            )}
            {umbral?.alarm_low != null && (
              <ReferenceArea
                y1={dominio ? Math.min(dominio[0], umbral.alarm_low) : umbral.alarm_low}
                y2={umbral.alarm_low}
                fill="var(--status-critical)"
                fillOpacity={0.07}
                ifOverflow="hidden"
              />
            )}

            <CartesianGrid stroke="var(--grid)" strokeWidth={1} vertical={false} />

            <XAxis
              dataKey="t"
              type="number"
              scale="time"
              domain={["dataMin", "dataMax"]}
              tickFormatter={formatearEjeX}
              stroke="var(--axis)"
              tick={{ fill: "var(--text-muted)", fontSize: 11 }}
              tickLine={false}
              minTickGap={48}
            />
            <YAxis
              domain={dominio ?? ["auto", "auto"]}
              ticks={escala?.marcas}
              stroke="var(--axis)"
              tick={{ fill: "var(--text-muted)", fontSize: 11 }}
              tickLine={false}
              axisLine={false}
              width={52}
              tickFormatter={(v: number) => v.toFixed(escala?.decimales ?? 0)}
            />

            {/* El guion aquí SÍ significa "umbral"; no es ruido sobre la rejilla. */}
            {umbral?.warn_high != null && (
              <ReferenceLine
                y={umbral.warn_high}
                stroke="var(--status-warning)"
                strokeDasharray="4 4"
                strokeWidth={1.5}
                ifOverflow="hidden"
                label={{ value: "Advertencia", position: "right",
                         fill: "var(--text-muted)", fontSize: 10 }}
              />
            )}
            {umbral?.alarm_high != null && (
              <ReferenceLine
                y={umbral.alarm_high}
                stroke="var(--status-critical)"
                strokeDasharray="4 4"
                strokeWidth={1.5}
                ifOverflow="hidden"
                label={{ value: "Alarma", position: "right",
                         fill: "var(--text-muted)", fontSize: 10 }}
              />
            )}

            <Tooltip
              content={<Etiqueta unidad={unidad} decimales={decimales} rangoLargo={rangoLargo} />}
              cursor={{ stroke: "var(--axis)", strokeWidth: 1 }}
            />

            {series.map((s) => (
              <Line
                key={String(s.clave)}
                type="monotone"
                dataKey={s.clave as string}
                name={s.nombre}
                stroke={s.color}
                strokeWidth={2}
                dot={false}
                activeDot={{ r: 4, strokeWidth: 2, stroke: "var(--surface-1)" }}
                isAnimationActive={false}
                connectNulls={false}
              />
            ))}
          </LineChart>
        </ResponsiveContainer>
      </div>
    </section>
  );
}

// ----------------------------------------------------------------------------
interface PropsEtiqueta {
  active?: boolean;
  payload?: Array<{ name: string; value: number | null; color: string }>;
  label?: number;
  unidad: string;
  decimales: number;
  rangoLargo: boolean;
}

function Etiqueta({ active, payload, label, unidad, decimales, rangoLargo }: PropsEtiqueta) {
  if (!active || !payload?.length) return null;

  const fecha = typeof label === "number" ? new Date(label) : null;
  const cuando = fecha
    ? rangoLargo
      ? `${FMT_FECHA.format(fecha)} ${FMT_HORA.format(fecha)}`
      : FMT_HORA.format(fecha)
    : "";

  return (
    <div
      className="rounded-lg px-3 py-2 text-xs shadow-lg"
      style={{
        background: "var(--surface-1)",
        border: "1px solid var(--border)",
        color: "var(--text-primary)",
      }}
    >
      <div className="mb-1 font-medium" style={{ color: "var(--text-muted)" }}>{cuando}</div>
      {payload.map((p) => (
        <div key={p.name} className="flex items-center gap-2 py-0.5">
          <span
            aria-hidden="true"
            className="inline-block rounded-sm"
            style={{ width: 10, height: 2, background: p.color }}
          />
          <span style={{ color: "var(--text-secondary)" }}>{p.name}</span>
          <span className="ml-auto font-medium tabular-nums">
            {p.value === null || p.value === undefined
              ? "sin dato"
              : `${p.value.toFixed(decimales)} ${unidad}`}
          </span>
        </div>
      ))}
    </div>
  );
}
