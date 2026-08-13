// ============================================================================
// Panel de conexión del equipo y controles de operación
// ============================================================================
// Responde a una pregunta concreta: ¿el ESP32 está llegando a internet?
//
// Distingue tres cosas que se confunden con facilidad y llevan a diagnosticar
// el problema equivocado:
//   · el equipo nunca ha reportado      -> falta firmware o configuración
//   · el equipo reportó y dejó de hacerlo -> se cayó la red o el equipo
//   · mi navegador perdió el enlace     -> problema de quien mira, no de la planta
// ============================================================================

import { useState } from "react";
import type { ComandoEquipo, EstadoEnlace, EstadoEquipo } from "../tipos";
import { formatearUptime, haceCuanto } from "../logica/estado";

interface Props {
  equipo: EstadoEquipo | null;
  enlace: EstadoEnlace;
  ahoraMs: number;
  esDemo: boolean;
  comandos: ComandoEquipo[];
  alEnviarComando: (
    comando: "tara" | "calibrar",
    pin: string,
    quien: string,
    parametros?: Record<string, unknown>,
  ) => Promise<{ ok: boolean; mensaje: string }>;
}

/** Calidad del enlace WiFi a partir del RSSI, en lenguaje llano. */
function calidadSenal(rssi: number | null): { texto: string; color: string; barras: number } {
  if (rssi === null) return { texto: "sin dato", color: "var(--text-muted)", barras: 0 };
  if (rssi >= -55) return { texto: "excelente", color: "var(--status-good)", barras: 4 };
  if (rssi >= -67) return { texto: "buena", color: "var(--status-good)", barras: 3 };
  // Por debajo de -75 dBm el ESP32 empieza a perder paquetes y a reconectar:
  // es el punto donde conviene mover la antena antes de que cause huecos.
  if (rssi >= -75) return { texto: "regular", color: "var(--status-warning)", barras: 2 };
  return { texto: "débil", color: "var(--status-critical)", barras: 1 };
}

function Barras({ n, color }: { n: number; color: string }) {
  return (
    <span className="inline-flex items-end gap-[2px]" aria-hidden="true">
      {[1, 2, 3, 4].map((i) => (
        <span
          key={i}
          style={{
            width: 3,
            height: 3 + i * 2.5,
            borderRadius: 1,
            background: i <= n ? color : "var(--axis)",
          }}
        />
      ))}
    </span>
  );
}

export function PanelEquipo({
  equipo, enlace, ahoraMs, esDemo, comandos, alEnviarComando,
}: Props) {
  const senal = calidadSenal(equipo?.rssi ?? null);
  const nuncaVisto = !equipo?.last_seen_at;
  const conectado = equipo?.estado_conexion === "online";

  return (
    <section
      className="rounded-xl p-4 flex flex-col gap-4"
      style={{ background: "var(--surface-1)", border: "1px solid var(--border)" }}
    >
      <div className="flex flex-wrap items-start justify-between gap-3">
        <h3 className="text-sm font-semibold">Conexión del equipo</h3>
        {esDemo && (
          <span
            className="text-[11px] rounded-full px-2 py-0.5"
            style={{
              color: "var(--status-warning)",
              background: "color-mix(in srgb, var(--status-warning) 14%, transparent)",
            }}
          >
            simulación
          </span>
        )}
      </div>

      {/* ------------------------------------------------------------------ */}
      {esDemo ? (
        <AvisoDemo />
      ) : nuncaVisto ? (
        <AvisoNuncaConectado />
      ) : (
        <div className="grid gap-3 sm:grid-cols-2 lg:grid-cols-4">
          <Metrica
            etiqueta="Internet"
            valor={conectado ? "Conectado" : "Sin conexión"}
            color={conectado ? "var(--status-good)" : "var(--status-critical)"}
            icono={conectado ? "●" : "○"}
            detalle={`última señal ${haceCuanto(equipo?.last_seen_at ?? null, ahoraMs)}`}
          />
          <Metrica
            etiqueta="Señal WiFi"
            valor={equipo?.rssi != null ? `${equipo.rssi} dBm` : "—"}
            color={senal.color}
            adorno={<Barras n={senal.barras} color={senal.color} />}
            detalle={senal.texto}
          />
          <Metrica
            etiqueta="Tiempo encendido"
            valor={formatearUptime(equipo?.uptime_s ?? null)}
            color="var(--text-primary)"
            detalle={`${equipo?.reconnects ?? 0} reconexiones`}
          />
          <Metrica
            etiqueta="Memoria libre"
            valor={equipo?.free_heap != null ? `${Math.round(equipo.free_heap / 1024)} KB` : "—"}
            color="var(--text-primary)"
            detalle={`firmware ${equipo?.fw_version ?? "—"}`}
          />
        </div>
      )}

      {enlace === "degradado" && !esDemo && (
        <p className="text-xs flex items-center gap-1.5" style={{ color: "var(--status-serious)" }}>
          <span aria-hidden="true">⚠</span>
          Tu navegador perdió el enlace en vivo y está consultando cada 10 s. El equipo puede
          estar perfectamente; esto es de tu lado.
        </p>
      )}

      <div style={{ borderTop: "1px solid var(--border)" }} />

      <ControlBascula
        comandos={comandos}
        ahoraMs={ahoraMs}
        deshabilitado={esDemo || nuncaVisto}
        motivoDeshabilitado={
          esDemo
            ? "No hay equipo real conectado: esto es una simulación."
            : "El equipo nunca ha reportado. Conéctalo antes de enviar órdenes."
        }
        alEnviarComando={alEnviarComando}
      />
    </section>
  );
}

// ----------------------------------------------------------------------------
function Metrica({
  etiqueta, valor, color, detalle, icono, adorno,
}: {
  etiqueta: string; valor: string; color: string;
  detalle?: string; icono?: string; adorno?: React.ReactNode;
}) {
  return (
    <div className="flex flex-col gap-0.5">
      <span className="text-[11px]" style={{ color: "var(--text-muted)" }}>{etiqueta}</span>
      <span className="flex items-center gap-1.5 text-sm font-semibold" style={{ color }}>
        {icono && <span aria-hidden="true">{icono}</span>}
        {adorno}
        {valor}
      </span>
      {detalle && (
        <span className="text-[11px]" style={{ color: "var(--text-muted)" }}>{detalle}</span>
      )}
    </div>
  );
}

// ----------------------------------------------------------------------------
function AvisoDemo() {
  return (
    <div
      className="rounded-lg p-3 text-xs flex flex-col gap-1"
      style={{
        background: "color-mix(in srgb, var(--status-warning) 8%, transparent)",
        border: "1px solid color-mix(in srgb, var(--status-warning) 28%, transparent)",
      }}
    >
      <p style={{ color: "var(--text-primary)" }}>
        <b>No hay ningún equipo conectado.</b> Lo que ves son datos generados por el navegador
        para poder evaluar la interfaz.
      </p>
      <p style={{ color: "var(--text-secondary)" }}>
        Para conectar el ESP32 real faltan dos cosas: cargarle el firmware de red (fases 2 y 3)
        y configurar las variables de Supabase en el repositorio.
      </p>
    </div>
  );
}

function AvisoNuncaConectado() {
  return (
    <div
      className="rounded-lg p-3 text-xs flex flex-col gap-2"
      style={{
        background: "color-mix(in srgb, var(--status-critical) 8%, transparent)",
        border: "1px solid color-mix(in srgb, var(--status-critical) 28%, transparent)",
      }}
    >
      <p style={{ color: "var(--text-primary)" }}>
        <b>El equipo nunca ha reportado.</b> Revisa en este orden:
      </p>
      <ol className="list-decimal ml-4 flex flex-col gap-0.5" style={{ color: "var(--text-secondary)" }}>
        <li>¿El firmware que tiene cargado publica a la nube? El sketch de pruebas solo imprime por puerto serie.</li>
        <li>¿Se conectó al WiFi? Míralo en el monitor serie a 115200 baudios.</li>
        <li>¿La red deja salir tráfico HTTPS al puerto 443 sin proxy?</li>
        <li>¿El token de ingesta del equipo coincide con el registrado?</li>
      </ol>
    </div>
  );
}

// ----------------------------------------------------------------------------
// Control de la báscula
// ----------------------------------------------------------------------------
function ControlBascula({
  comandos, ahoraMs, deshabilitado, motivoDeshabilitado, alEnviarComando,
}: {
  comandos: ComandoEquipo[];
  ahoraMs: number;
  deshabilitado: boolean;
  motivoDeshabilitado: string;
  alEnviarComando: Props["alEnviarComando"];
}) {
  const [abierto, setAbierto] = useState(false);
  const [pin, setPin] = useState("");
  const [quien, setQuien] = useState("");
  const [enviando, setEnviando] = useState(false);
  const [aviso, setAviso] = useState<{ ok: boolean; texto: string } | null>(null);

  const enCurso = comandos.find(
    (c) => c.comando === "tara" && (c.estado === "pendiente" || c.estado === "entregado"),
  );
  const ultima = comandos.find((c) => c.comando === "tara" && c.estado === "ejecutado");

  async function enviar(e: React.FormEvent) {
    e.preventDefault();
    setEnviando(true);
    setAviso(null);
    const r = await alEnviarComando("tara", pin, quien);
    setEnviando(false);
    setAviso({ ok: r.ok, texto: r.mensaje });
    if (r.ok) {
      setAbierto(false);
      setPin("");
    }
  }

  return (
    <div className="flex flex-col gap-2">
      <div className="flex flex-wrap items-center gap-3">
        <div className="mr-auto">
          <h4 className="text-xs font-semibold">Báscula</h4>
          <p className="text-[11px]" style={{ color: "var(--text-muted)" }}>
            {enCurso
              ? "Orden de tara en camino…"
              : ultima
                ? `Última tara ${haceCuanto(ultima.ejecutado_at, ahoraMs)} por ${ultima.solicitado_por}`
                : "Poner en cero con la báscula vacía"}
          </p>
        </div>

        <button
          onClick={() => setAbierto((v) => !v)}
          disabled={deshabilitado || Boolean(enCurso)}
          title={deshabilitado ? motivoDeshabilitado : undefined}
          className="text-xs font-medium rounded-lg px-3 py-1.5 transition-opacity
                     hover:opacity-80 disabled:opacity-40 disabled:cursor-not-allowed"
          style={{ border: "1px solid var(--border)", color: "var(--text-secondary)" }}
        >
          {enCurso ? "⏳ Tara en curso" : "⊘ Tarar báscula"}
        </button>
      </div>

      {deshabilitado && (
        <p className="text-[11px]" style={{ color: "var(--text-muted)" }}>
          {motivoDeshabilitado}
        </p>
      )}

      {abierto && !deshabilitado && (
        <form
          onSubmit={enviar}
          className="rounded-lg p-3 flex flex-col gap-2"
          style={{ background: "var(--page)", border: "1px solid var(--border)" }}
        >
          <p className="text-[11px]" style={{ color: "var(--text-secondary)" }}>
            La tara pone el peso actual como cero. Hazlo con la báscula <b>vacía</b>.
            Se aplicará en la próxima sincronización del equipo (máx. 30 s).
          </p>
          <div className="flex flex-wrap gap-2">
            <input
              required minLength={2} maxLength={60}
              value={quien} onChange={(e) => setQuien(e.target.value)}
              placeholder="Tu nombre"
              className="text-xs rounded px-2 py-1.5 flex-1 min-w-[120px]"
              style={{
                border: "1px solid var(--border)",
                background: "var(--surface-1)", color: "var(--text-primary)",
              }}
            />
            <input
              required minLength={4} type="password" inputMode="numeric"
              value={pin} onChange={(e) => setPin(e.target.value)}
              placeholder="PIN de operador"
              autoComplete="off"
              className="text-xs rounded px-2 py-1.5 w-[140px]"
              style={{
                border: "1px solid var(--border)",
                background: "var(--surface-1)", color: "var(--text-primary)",
              }}
            />
            <button
              type="submit" disabled={enviando}
              className="text-xs font-medium rounded px-3 py-1.5 disabled:opacity-50"
              style={{ background: "var(--series-1)", color: "#fff" }}
            >
              {enviando ? "Enviando…" : "Confirmar tara"}
            </button>
          </div>
          <p className="text-[11px]" style={{ color: "var(--text-muted)" }}>
            El PIN se valida en el servidor y nunca se guarda en esta página. Esta pantalla es
            pública: sin él, cualquiera podría poner la báscula en cero a media producción.
          </p>
        </form>
      )}

      {aviso && (
        <p
          className="text-[11px] flex items-start gap-1.5"
          style={{ color: aviso.ok ? "var(--status-good)" : "var(--status-critical)" }}
          role="status"
        >
          <span aria-hidden="true">{aviso.ok ? "✓" : "✕"}</span>
          {aviso.texto}
        </p>
      )}
    </div>
  );
}
