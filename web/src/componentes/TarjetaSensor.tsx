// ============================================================================
// Tarjeta de un canal — el elemento principal de la vista general
// ============================================================================
// Es una "stat tile", no una gráfica: la historia es UN número. El sparkline
// aporta contexto de tendencia, no valores legibles, y por eso no lleva ejes ni
// etiquetas.
// ============================================================================

import { useId } from "react";
import type { DefinicionSensor } from "../tipos";
import type { EvaluacionCanal } from "../logica/estado";
import { colorSeveridad, formatear } from "../logica/estado";

interface Props {
  sensor: DefinicionSensor;
  evaluacion: EvaluacionCanal;
  /** Últimos ~5 min del canal, para la tendencia. */
  serie: (number | null)[];
}

function Sparkline({ puntos, color }: { puntos: (number | null)[]; color: string }) {
  const idGrad = useId();
  const validos = puntos.filter((p): p is number => p !== null);

  if (validos.length < 2) {
    return <div className="h-10" aria-hidden="true" />;
  }

  const min = Math.min(...validos);
  const max = Math.max(...validos);
  const rango = max - min || 1;
  const An = 100;
  const Al = 32;

  // Los huecos (null) parten la línea en tramos en vez de interpolarse: dibujar
  // una recta sobre un periodo sin dato inventaría una lectura que no existió.
  const tramos: string[] = [];
  let actual: string[] = [];
  puntos.forEach((p, i) => {
    if (p === null) {
      if (actual.length > 1) tramos.push(actual.join(" "));
      actual = [];
      return;
    }
    const x = (i / (puntos.length - 1)) * An;
    const y = Al - ((p - min) / rango) * (Al - 4) - 2;
    actual.push(`${x.toFixed(2)},${y.toFixed(2)}`);
  });
  if (actual.length > 1) tramos.push(actual.join(" "));

  const ultimo = puntos[puntos.length - 1];
  const cx = An;
  const cy = ultimo !== null ? Al - ((ultimo - min) / rango) * (Al - 4) - 2 : null;

  return (
    <svg
      viewBox={`0 0 ${An} ${Al}`}
      preserveAspectRatio="none"
      className="h-10 w-full"
      aria-hidden="true"
      focusable="false"
    >
      <defs>
        <linearGradient id={idGrad} x1="0" y1="0" x2="0" y2="1">
          <stop offset="0%" stopColor={color} stopOpacity="0.18" />
          <stop offset="100%" stopColor={color} stopOpacity="0" />
        </linearGradient>
      </defs>
      {tramos.length > 0 && (
        <polygon
          points={`0,${Al} ${tramos[0]} ${An},${Al}`}
          fill={`url(#${idGrad})`}
        />
      )}
      {tramos.map((t, i) => (
        <polyline
          key={i}
          points={t}
          fill="none"
          stroke={color}
          strokeWidth="1.5"
          strokeLinecap="round"
          strokeLinejoin="round"
          vectorEffect="non-scaling-stroke"
        />
      ))}
      {cy !== null && (
        <circle cx={cx} cy={cy} r="2" fill={color} vectorEffect="non-scaling-stroke" />
      )}
    </svg>
  );
}

export function TarjetaSensor({ sensor, evaluacion, serie }: Props) {
  const color = colorSeveridad(evaluacion.severidad, evaluacion.estado);
  const hayProblema = evaluacion.severidad !== "normal";

  return (
    <article
      className="rounded-xl p-4 flex flex-col gap-3"
      style={{
        background: "var(--surface-1)",
        border: "1px solid var(--border)",
      }}
    >
      {/* flex-wrap y no solo justify-between: en una tarjeta angosta la insignia
          se pasa a su propio renglón en vez de encimarse con el título. */}
      <header className="flex flex-wrap items-start justify-between gap-x-2 gap-y-1">
        <h3
          className="text-[13px] font-medium leading-tight"
          style={{ color: "var(--text-secondary)" }}
        >
          {sensor.etiqueta}
        </h3>
        {/* Icono + texto acompañan siempre al color: el estado nunca se
            comunica solo con el tono. */}
        <span
          className="shrink-0 inline-flex items-center gap-1 text-[11px] font-medium
                     rounded-full px-2 py-0.5"
          style={{
            color: hayProblema ? color : "var(--text-muted)",
            background: hayProblema ? `color-mix(in srgb, ${color} 12%, transparent)` : "transparent",
          }}
        >
          <span aria-hidden="true">{evaluacion.icono}</span>
          {evaluacion.etiquetaCorta}
        </span>
      </header>

      <div className="flex items-baseline gap-1.5">
        {/* Figuras proporcionales, no tabulares: en tamaño grande los dígitos
            de ancho fijo hacen que el número se vea suelto. */}
        <span
          className="text-[34px] leading-none font-semibold tracking-tight"
          style={{ color: evaluacion.valor === null ? "var(--text-muted)" : "var(--text-primary)" }}
        >
          {formatear(evaluacion.valor, sensor.decimales)}
        </span>
        <span className="text-sm" style={{ color: "var(--text-muted)" }}>
          {sensor.unidad}
        </span>
      </div>

      <Sparkline puntos={serie} color={color} />

      <span className="sr-only">
        {sensor.etiqueta}: {formatear(evaluacion.valor, sensor.decimales)} {sensor.unidad}.
        Estado: {evaluacion.etiquetaEstado}.
      </span>
    </article>
  );
}
