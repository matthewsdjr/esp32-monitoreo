import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import type { FuenteDatos } from "./datos";
import { crearFuente } from "./datos";
import type {
  Alarma, ComandoEquipo, DefinicionSensor, EstadoEnlace, EstadoEquipo,
  Lectura, RangoHistorico, Umbral,
} from "./tipos";
import { evaluarCanal, msObsoletoSegun, valorDe } from "./logica/estado";
import { BarraEstado } from "./componentes/BarraEstado";
import { TarjetaSensor } from "./componentes/TarjetaSensor";
import { PanelHistorico } from "./componentes/PanelHistorico";
import { TablaAlarmas } from "./componentes/TablaAlarmas";
import { PanelEquipo } from "./componentes/PanelEquipo";

/** Muestras conservadas para los sparklines: 150 × 2 s = 5 min de tendencia. */
const LARGO_SPARKLINE = 150;

type Tema = "claro" | "oscuro";

function temaInicial(): Tema {
  const guardado = localStorage.getItem("tema");
  if (guardado === "claro" || guardado === "oscuro") return guardado;
  return window.matchMedia("(prefers-color-scheme: dark)").matches ? "oscuro" : "claro";
}

export default function App() {
  const [fuente, setFuente] = useState<FuenteDatos | null>(null);
  const [equipo, setEquipo] = useState<EstadoEquipo | null>(null);
  const [sensores, setSensores] = useState<DefinicionSensor[]>([]);
  const [umbrales, setUmbrales] = useState<Umbral[]>([]);
  const [alarmas, setAlarmas] = useState<Alarma[]>([]);
  const [comandos, setComandos] = useState<ComandoEquipo[]>([]);
  const [ultima, setUltima] = useState<Lectura | null>(null);
  const [enlace, setEnlace] = useState<EstadoEnlace>("desconectado");
  const [historico, setHistorico] = useState<Lectura[]>([]);
  const [rango, setRango] = useState<RangoHistorico>("6h");
  const [cargandoHist, setCargandoHist] = useState(false);
  const [ahoraMs, setAhoraMs] = useState(() => Date.now());
  const [tema, setTema] = useState<Tema>(temaInicial);

  // Ventana rodante para los sparklines
  const recientes = useRef<Lectura[]>([]);
  const [, forzarRender] = useState(0);

  // --- Tema ------------------------------------------------------------------
  useEffect(() => {
    document.documentElement.setAttribute("data-theme", tema === "oscuro" ? "dark" : "light");
    localStorage.setItem("tema", tema);
  }, [tema]);

  // --- Reloj -----------------------------------------------------------------
  // Un reloj propio, y no Date.now() dentro del render: "hace N s" y la
  // detección de dato obsoleto deben avanzar aunque no lleguen lecturas nuevas.
  // Es justamente cuando NO llega nada que el usuario necesita verlo.
  useEffect(() => {
    const id = setInterval(() => setAhoraMs(Date.now()), 1000);
    return () => clearInterval(id);
  }, []);

  // --- Arranque --------------------------------------------------------------
  useEffect(() => {
    let vivo = true;
    crearFuente().then(async (f) => {
      if (!vivo) return;
      setFuente(f);
      const [eq, sen, umb, ala, cmd] = await Promise.all([
        f.cargarEquipo(), f.cargarSensores(), f.cargarUmbrales(),
        f.cargarAlarmas(), f.cargarComandos(),
      ]);
      if (!vivo) return;
      setEquipo(eq); setSensores(sen); setUmbrales(umb); setAlarmas(ala); setComandos(cmd);
    });
    return () => { vivo = false; };
  }, []);

  // --- Suscripción en vivo ---------------------------------------------------
  useEffect(() => {
    if (!fuente) return;
    return fuente.suscribir(
      (l) => {
        setUltima(l);
        const arr = recientes.current;
        arr.push(l);
        if (arr.length > LARGO_SPARKLINE) arr.shift();
        forzarRender((n) => n + 1);
      },
      setEnlace,
    );
  }, [fuente]);

  // --- Histórico -------------------------------------------------------------
  useEffect(() => {
    if (!fuente) return;
    let vivo = true;
    setCargandoHist(true);
    fuente.cargarHistorico(rango).then((d) => {
      if (!vivo) return;
      setHistorico(d);
      setCargandoHist(false);
    });
    return () => { vivo = false; };
  }, [fuente, rango]);

  // Refresco periódico del estado del equipo, alarmas y comandos.
  // Con un comando en vuelo el sondeo baja a 5 s: el operador está mirando la
  // pantalla esperando el acuse, y 30 s de silencio se leen como que falló.
  const hayComandoEnVuelo = comandos.some(
    (c) => c.estado === "pendiente" || c.estado === "entregado",
  );

  useEffect(() => {
    if (!fuente) return;
    const periodo = hayComandoEnVuelo ? 5_000 : 30_000;
    const id = setInterval(async () => {
      const [eq, ala, cmd] = await Promise.all([
        fuente.cargarEquipo(), fuente.cargarAlarmas(), fuente.cargarComandos(),
      ]);
      setEquipo(eq); setAlarmas(ala); setComandos(cmd);
    }, periodo);
    return () => clearInterval(id);
  }, [fuente, hayComandoEnVuelo]);

  // --- Derivados -------------------------------------------------------------
  const umbralPor = useMemo(
    () => Object.fromEntries(umbrales.map((u) => [u.canal, u])),
    [umbrales],
  );

  const evaluaciones = useMemo(
    () => sensores.map((s) => ({
      sensor: s,
      evaluacion: evaluarCanal(s, ultima, umbralPor[s.slug], ahoraMs,
                               msObsoletoSegun(enlace)),
      serie: recientes.current.map((l) => valorDe(l, s.slug)),
    })),
    // recientes.current muta por referencia; forzarRender es lo que dispara el
    // recálculo, y ahoraMs entra porque la evaluación depende de la antigüedad.
    // eslint-disable-next-line react-hooks/exhaustive-deps
    [sensores, ultima, umbralPor, ahoraMs, enlace],
  );

  const reconocer = useCallback(async (id: number, por: string) => {
    setAlarmas((prev) =>
      prev.map((a) => (a.id === id ? { ...a, reconocida_por: por } : a)));
    try {
      await fuente?.reconocerAlarma(id, por);
    } catch (e) {
      console.error("No se pudo reconocer la alarma", e);
      if (fuente) setAlarmas(await fuente.cargarAlarmas());
    }
  }, [fuente]);

  const enviarComando = useCallback(
    async (
      comando: "tara" | "calibrar",
      pin: string,
      quien: string,
      parametros?: Record<string, unknown>,
    ) => {
      if (!fuente) return { ok: false, mensaje: "Sin conexión con el servidor." };
      const r = await fuente.enviarComando(comando, pin, quien, parametros);
      if (r.ok) setComandos(await fuente.cargarComandos());
      return r;
    },
    [fuente],
  );

  return (
    <div className="min-h-full p-3 sm:p-5">
      <div className="mx-auto max-w-[1400px] flex flex-col gap-4">
        <BarraEstado
          equipo={equipo}
          enlace={enlace}
          ahoraMs={ahoraMs}
          tema={tema}
          alCambiarTema={() => setTema((t) => (t === "claro" ? "oscuro" : "claro"))}
        />

        <section
          className="grid gap-3 grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 2xl:grid-cols-5"
          aria-label="Estado actual de los sensores"
        >
          {evaluaciones.map(({ sensor, evaluacion, serie }) => (
            <TarjetaSensor
              key={sensor.slug}
              sensor={sensor}
              evaluacion={evaluacion}
              serie={serie}
            />
          ))}
        </section>

        <PanelEquipo
          equipo={equipo}
          enlace={enlace}
          ahoraMs={ahoraMs}
          esDemo={fuente?.esDemo ?? false}
          comandos={comandos}
          alEnviarComando={enviarComando}
        />

        <TablaAlarmas
          alarmas={alarmas}
          ahoraMs={ahoraMs}
          alReconocer={reconocer}
          soloLectura={false}
        />

        <PanelHistorico
          datos={historico}
          umbrales={umbrales}
          rango={rango}
          alCambiarRango={setRango}
          cargando={cargandoHist}
        />

        <footer
          className="text-[11px] text-center pt-1 pb-3"
          style={{ color: "var(--text-muted)" }}
        >
          {fuente?.esDemo
            ? "Modo demostración · datos simulados en el navegador. Configura VITE_SUPABASE_URL para conectar el equipo real."
            : `Equipo ${equipo?.slug ?? ""} · datos en vivo`}
        </footer>
      </div>
    </div>
  );
}
