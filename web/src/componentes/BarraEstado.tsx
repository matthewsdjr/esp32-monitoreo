// ============================================================================
// Barra superior: identidad del equipo y salud del enlace
// ============================================================================
// Distingue explícitamente DOS fallas que suelen confundirse:
//   · el equipo está caído          -> estado_conexion del servidor
//   · mi navegador perdió el enlace -> estado del WebSocket
// Mezclarlas produce falsas alarmas ("el horno se apagó" cuando en realidad se
// cayó el wifi de quien mira la pantalla).
// ============================================================================

import type { EstadoEnlace, EstadoEquipo } from "../tipos";
import { formatearUptime, haceCuanto } from "../logica/estado";

interface Props {
  equipo: EstadoEquipo | null;
  enlace: EstadoEnlace;
  ahoraMs: number;
  tema: "claro" | "oscuro";
  alCambiarTema: () => void;
}

const ESTADO_EQUIPO: Record<
  EstadoEquipo["estado_conexion"],
  { texto: string; color: string; icono: string }
> = {
  online:       { texto: "En línea",     color: "var(--status-good)",     icono: "●" },
  intermitente: { texto: "Intermitente", color: "var(--status-warning)",  icono: "◐" },
  offline:      { texto: "Fuera de línea", color: "var(--status-critical)", icono: "○" },
  nunca_visto:  { texto: "Nunca conectado", color: "var(--text-muted)",   icono: "○" },
};

const ESTADO_ENLACE: Record<EstadoEnlace, { texto: string; color: string } | null> = {
  vivo: null, // sin novedad: no se muestra nada
  demo: { texto: "Datos simulados", color: "var(--status-warning)" },
  degradado: { texto: "Enlace degradado · actualizando cada 10 s", color: "var(--status-serious)" },
  desconectado: { texto: "Sin conexión con el servidor", color: "var(--status-critical)" },
};

export function BarraEstado({ equipo, enlace, ahoraMs, tema, alCambiarTema }: Props) {
  const est = equipo ? ESTADO_EQUIPO[equipo.estado_conexion] : null;
  const avisoEnlace = ESTADO_ENLACE[enlace];

  return (
    <header
      className="rounded-xl px-4 py-3 flex flex-wrap items-center gap-x-6 gap-y-3"
      style={{ background: "var(--surface-1)", border: "1px solid var(--border)" }}
    >
      <div className="flex items-center gap-3 mr-auto">
        <div>
          <h1 className="text-base font-semibold leading-tight">
            {equipo?.nombre ?? "Cargando…"}
          </h1>
          <p className="text-xs" style={{ color: "var(--text-muted)" }}>
            {equipo?.ubicacion ?? "—"}
          </p>
        </div>

        {est && (
          <span
            className="inline-flex items-center gap-1.5 text-xs font-medium rounded-full px-2.5 py-1"
            style={{
              color: est.color,
              background: `color-mix(in srgb, ${est.color} 12%, transparent)`,
            }}
          >
            <span aria-hidden="true">{est.icono}</span>
            {est.texto}
          </span>
        )}
      </div>

      <dl className="flex items-center gap-x-5 gap-y-1 flex-wrap text-xs">
        <Dato etiqueta="Última lectura" valor={haceCuanto(equipo?.last_seen_at ?? null, ahoraMs)} />
        <Dato etiqueta="Señal" valor={equipo?.rssi != null ? `${equipo.rssi} dBm` : "—"} />
        <Dato etiqueta="Activo" valor={formatearUptime(equipo?.uptime_s ?? null)} />
        <Dato etiqueta="Firmware" valor={equipo?.fw_version ?? "—"} />
      </dl>

      <button
        onClick={alCambiarTema}
        className="text-xs rounded-lg px-2.5 py-1.5 transition-opacity hover:opacity-70"
        style={{ border: "1px solid var(--border)", color: "var(--text-secondary)" }}
        aria-label={`Cambiar a tema ${tema === "claro" ? "oscuro" : "claro"}`}
      >
        {tema === "claro" ? "◐ Oscuro" : "◑ Claro"}
      </button>

      {avisoEnlace && (
        <p
          className="basis-full text-xs flex items-center gap-1.5 pt-1"
          style={{ color: avisoEnlace.color }}
          role="status"
        >
          <span aria-hidden="true">⚠</span>
          {avisoEnlace.texto}
        </p>
      )}
    </header>
  );
}

function Dato({ etiqueta, valor }: { etiqueta: string; valor: string }) {
  return (
    <div className="flex flex-col">
      <dt style={{ color: "var(--text-muted)" }}>{etiqueta}</dt>
      <dd className="font-medium tabular-nums" style={{ color: "var(--text-secondary)" }}>
        {valor}
      </dd>
    </div>
  );
}
