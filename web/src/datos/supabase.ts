// ============================================================================
// Fuente de datos REAL — Supabase
// ============================================================================

import { createClient, type RealtimeChannel, type SupabaseClient } from "@supabase/supabase-js";
import type { FuenteDatos } from "./fuente";
import type {
  Alarma,
  Canal,
  DefinicionSensor,
  EstadoEnlace,
  EstadoEquipo,
  Lectura,
  RangoHistorico,
  Umbral,
} from "../tipos";
import { RANGOS, UMBRAL_AGREGADO_MS } from "../tipos";

const URL = import.meta.env.VITE_SUPABASE_URL as string;
const ANON = import.meta.env.VITE_SUPABASE_ANON_KEY as string;
const SLUG = (import.meta.env.VITE_DEVICE_SLUG as string) || "planta-01";

export class FuenteSupabase implements FuenteDatos {
  readonly esDemo = false;
  private db: SupabaseClient;
  private canal: RealtimeChannel | null = null;
  private idEncuesta: number | null = null;
  private deviceId: string | null = null;

  constructor() {
    this.db = createClient(URL, ANON, {
      realtime: { params: { eventsPerSecond: 2 } },
    });
  }

  private async idEquipo(): Promise<string> {
    if (this.deviceId) return this.deviceId;
    const { data } = await this.db
      .from("devices_publico").select("id").eq("slug", SLUG).maybeSingle();
    this.deviceId = (data?.id as string | undefined) ?? "";
    return this.deviceId;
  }

  // --------------------------------------------------------------------------
  suscribir(
    alRecibirLectura: (l: Lectura) => void,
    alCambiarEnlace: (e: EstadoEnlace) => void,
  ): () => void {
    this.canal = this.db
      .channel(`telemetria:${SLUG}`, { config: { private: true } })
      .on("broadcast", { event: "lectura" }, ({ payload }) => {
        alRecibirLectura(payload as Lectura);
        this.detenerEncuesta();
        alCambiarEnlace("vivo");
      })
      .subscribe((estado) => {
        if (estado === "SUBSCRIBED") {
          alCambiarEnlace("vivo");
          this.detenerEncuesta();
        } else if (estado === "CHANNEL_ERROR" || estado === "TIMED_OUT" || estado === "CLOSED") {
          // Degradación, no caída: se sigue mostrando dato fresco por REST cada
          // 10 s mientras el WebSocket se recupera. El usuario ve un aviso
          // discreto en vez de una pantalla congelada sin explicación.
          alCambiarEnlace("degradado");
          this.iniciarEncuesta(alRecibirLectura);
        }
      });

    return () => {
      this.detenerEncuesta();
      if (this.canal) this.db.removeChannel(this.canal);
    };
  }

  private iniciarEncuesta(cb: (l: Lectura) => void) {
    if (this.idEncuesta !== null) return;
    const tick = async () => {
      const id = await this.idEquipo();
      const { data } = await this.db
        .from("latest_readings").select("payload").eq("device_id", id).maybeSingle();
      if (data?.payload) cb(data.payload as Lectura);
    };
    tick();
    this.idEncuesta = window.setInterval(tick, 10_000);
  }

  private detenerEncuesta() {
    if (this.idEncuesta !== null) {
      clearInterval(this.idEncuesta);
      this.idEncuesta = null;
    }
  }

  // --------------------------------------------------------------------------
  async cargarEquipo(): Promise<EstadoEquipo> {
    const { data, error } = await this.db
      .from("devices_publico").select("*").eq("slug", SLUG).maybeSingle();
    if (error || !data) throw new Error(error?.message ?? "equipo no encontrado");
    this.deviceId = data.id;
    return data as EstadoEquipo;
  }

  async cargarSensores(): Promise<DefinicionSensor[]> {
    const id = await this.idEquipo();
    const { data } = await this.db
      .from("sensors")
      .select("slug, etiqueta, unidad, decimales, min_fisico, max_fisico")
      .eq("device_id", id).eq("activo", true).order("orden");
    return (data ?? []) as DefinicionSensor[];
  }

  async cargarUmbrales(): Promise<Umbral[]> {
    const { data } = await this.db
      .from("thresholds")
      .select("warn_low, warn_high, alarm_low, alarm_high, sensors!inner(slug, device_id)")
      .eq("activo", true);

    return (data ?? []).map((t: Record<string, unknown>) => ({
      canal: (t.sensors as { slug: Canal }).slug,
      warn_low: t.warn_low as number | null,
      warn_high: t.warn_high as number | null,
      alarm_low: t.alarm_low as number | null,
      alarm_high: t.alarm_high as number | null,
    }));
  }

  async cargarHistorico(rango: RangoHistorico): Promise<Lectura[]> {
    const id = await this.idEquipo();
    const ventana = RANGOS[rango].ms;
    const desde = new Date(Date.now() - ventana).toISOString();

    // Selección automática de fuente. Consultar `readings` para 30 días
    // descargaría ~500 000 filas al navegador.
    if (ventana <= UMBRAL_AGREGADO_MS) {
      const { data } = await this.db
        .from("readings")
        .select("ts, peso_g, temp_amb_c, hum_pct, tc1_c, tc2_c, faults")
        .eq("device_id", id).gte("ts", desde).order("ts")
        .limit(20_000);
      return (data ?? []) as Lectura[];
    }

    const { data } = await this.db
      .from("readings_5m")
      .select("bucket, peso_g_avg, temp_amb_c_avg, hum_pct_avg, tc1_c_avg, tc2_c_avg, n_fallas")
      .eq("device_id", id).gte("bucket", desde).order("bucket")
      .limit(20_000);

    return (data ?? []).map((r: Record<string, unknown>) => ({
      ts: r.bucket as string,
      peso_g: r.peso_g_avg as number | null,
      temp_amb_c: r.temp_amb_c_avg as number | null,
      hum_pct: r.hum_pct_avg as number | null,
      tc1_c: r.tc1_c_avg as number | null,
      tc2_c: r.tc2_c_avg as number | null,
      faults: (r.n_fallas as number) > 0 ? 1 : 0,
    }));
  }

  async cargarAlarmas(): Promise<Alarma[]> {
    const id = await this.idEquipo();
    const { data } = await this.db
      .from("alerts")
      .select("id, nivel, direccion, valor_disparo, valor_pico, umbral, abierta_at, cerrada_at, reconocida_por, sensors!inner(slug, etiqueta)")
      .eq("device_id", id).order("abierta_at", { ascending: false }).limit(100);

    return (data ?? []).map((a: Record<string, unknown>) => {
      const s = a.sensors as { slug: Canal; etiqueta: string };
      return {
        id: a.id as number,
        canal: s.slug,
        etiqueta: s.etiqueta,
        nivel: a.nivel as "warning" | "alarm",
        direccion: a.direccion as "low" | "high" | "falla",
        valor_disparo: a.valor_disparo as number | null,
        valor_pico: a.valor_pico as number | null,
        umbral: a.umbral as number | null,
        abierta_at: a.abierta_at as string,
        cerrada_at: a.cerrada_at as string | null,
        reconocida_por: a.reconocida_por as string | null,
      };
    });
  }

  async reconocerAlarma(id: number, por: string): Promise<void> {
    const { error } = await this.db.rpc("reconocer_alerta", { p_alert_id: id, p_por: por });
    if (error) throw new Error(error.message);
  }
}

// ----------------------------------------------------------------------------
export const hayBackendConfigurado = Boolean(URL && ANON);
