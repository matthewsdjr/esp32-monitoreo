// ============================================================================
// Monitoreo ESP32 — adquisición de sensores y publicación a la nube
// ============================================================================
// Qué cambia respecto al prototipo, y por qué importa:
//
//   ANTES                                  AHORA
//   loop() con delay(500)                  Tareas FreeRTOS con periodo fijo
//   get_units(10) bloquea ~1 s             Lectura solo cuando hay dato (~60 µs)
//   readCelsius() sin cadencia             250 ms garantizados por termopar
//   while(1) si falla el SHT31             El canal se degrada, el equipo sigue
//   serialEvent() (no existe en ESP32)     Consola atendida en su propia tarea
//   Sin persistencia                       Calibración y tara en NVS
//
// El reparto por núcleo es lo que sostiene la red: el núcleo 0 queda para la
// pila TLS, así que ninguna lectura lenta puede tumbar la conexión.
// ============================================================================

#include <Arduino.h>

#include "config.h"
#include "modelo/contrato.h"
#include "sensores/canal.h"
#include "sensores/bascula.h"
#include "sensores/ambiente.h"
#include "sensores/termopares.h"
#include "almacen/ajustes.h"
#include "almacen/bufer.h"
#include "red/credenciales.h"
#include "red/wifi.h"
#include "red/tiempo.h"
#include "red/publicador.h"
#include "red/servidor_local.h"
#include "red/ota.h"

#include <esp_task_wdt.h>

// ---------------------------------------------------------------------------
// Estado compartido
// ---------------------------------------------------------------------------
// Varias tareas escriben canales distintos y la tarea de agregado los lee todos.
// Un mutex protege el conjunto; las secciones críticas son de microsegundos.
static SemaphoreHandle_t mtxEstado;

static sensores::Canal canales[contrato::NUM_CANALES];
static sensores::Bascula bascula;
static sensores::Ambiente ambiente;
static sensores::Termopar termo1, termo2;
static almacen::Ajustes ajustes;

// --- Red ---------------------------------------------------------------------
static red::AlmacenCredenciales almacenCred;
static red::GestorWiFi gestorWifi;
static red::Tiempo reloj;
static red::Publicador publicador;
static red::ServidorLocal servidorLocal;
static red::Credenciales credenciales;

// 720 muestras a una cada 5 s = 1 hora en RAM (~20 KB). Más allá, respaldo en
// flash. El tope existe para dejar heap libre a TLS, que necesita ~45 KB.
static almacen::AnilloRam<720> anillo;
static almacen::RespaldoFlash respaldo;
static SemaphoreHandle_t mtxBufer;

// Acuses de comandos ya ejecutados, pendientes de enviar en el próximo lote.
static red::AcuseComando acuses[8];
static volatile size_t numAcuses = 0;
static SemaphoreHandle_t mtxAcuses;

static volatile bool credencialesInvalidas = false;

/** Última muestra consolidada. La Fase 3 la tomará de aquí para publicarla. */
static contrato::Muestra ultimaMuestra;
static SemaphoreHandle_t mtxMuestra;

// Diagnóstico
static uint32_t numArranque = 0;
static volatile uint32_t lecturasBascula = 0;
static volatile uint32_t fallosTermo1 = 0, fallosTermo2 = 0;

// Órdenes pendientes desde consola o (en la Fase 3) desde la nube.
static volatile bool solicitudTara = false;
static volatile float solicitudCalibrar = 0.0f;

// ---------------------------------------------------------------------------
static void imprimirDiagnostico();                       // usada por la consola
static void ejecutarComando(const red::ComandoPendiente&); // usada por el envío
static void enviarLotePendiente(uint32_t& esperaLoteMs);   // usada por la tarea de red
static red::EstadoServicio* obtenerEstadoServicio();        // usada por el servidor local
static void tareaLocal(void*);
static void aplicarSet(String resto);
static void imprimirConfig();
static void atenderSerieEnPortal();
static String prefsPassTemporal;

static inline sensores::Canal& canal(contrato::Canal c) {
  return canales[static_cast<uint8_t>(c)];
}

// ===========================================================================
// Tarea: báscula
// ===========================================================================
// Periodo de 20 ms frente a los 10 SPS del HX711: se sondea con holgura y la
// mayoría de las pasadas no encuentran dato y retornan de inmediato. Sondear
// rápido y salir es más barato que esperar bloqueando.
static void tareaBascula(void*) {
  TickType_t ultimo = xTaskGetTickCount();

  for (;;) {
    if (bascula.actualizar()) {
      lecturasBascula++;

      if (xSemaphoreTake(mtxEstado, pdMS_TO_TICKS(20)) == pdTRUE) {
        if (bascula.sospechaDesconexion()) {
          canal(contrato::Canal::PESO).publicarFalla(millis());
        } else {
          canal(contrato::Canal::PESO).publicar(bascula.gramos(), millis());
        }
        xSemaphoreGive(mtxEstado);
      }
    }

    // --- Órdenes pendientes -------------------------------------------------
    // Se atienden dentro de esta tarea, que es la dueña del objeto báscula:
    // así no hace falta un mutex adicional sobre el driver.
    if (solicitudTara) {
      solicitudTara = false;
      if (bascula.tarar()) {
        ajustes.guardarOffset(bascula.calibracion().offset(), 0);
        Serial.printf("[bascula] tara aplicada · offset=%d\n",
                      bascula.calibracion().offset());
      } else {
        Serial.println("[bascula] TARA RECHAZADA: la señal no está estable. "
                       "Espera a que se asiente y reintenta.");
      }
    }

    const float peso = solicitudCalibrar;
    if (peso > 0.0f) {
      solicitudCalibrar = 0.0f;
      if (bascula.calibrar(peso)) {
        ajustes.guardarFactor(bascula.calibracion().factor());
        Serial.printf("[bascula] calibrada · factor=%.3f cuentas/g\n",
                      bascula.calibracion().factor());
      } else {
        Serial.println("[bascula] CALIBRACIÓN RECHAZADA: señal inestable o el "
                       "peso patrón no está colocado.");
      }
    }

    vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(config::MS_TAREA_BASCULA));
  }
}

// ===========================================================================
// Tarea: ambiente (SHT31)
// ===========================================================================
static void tareaAmbiente(void*) {
  TickType_t ultimo = xTaskGetTickCount();

  for (;;) {
    float t = 0, h = 0;
    const bool ok = ambiente.leer(t, h);
    const uint32_t ahora = millis();

    if (xSemaphoreTake(mtxEstado, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (ok) {
        canal(contrato::Canal::TEMP_AMB).publicar(t, ahora);
        canal(contrato::Canal::HUM).publicar(h, ahora);
      } else {
        // Marca ambos canales, pero NO detiene el equipo. Ésta es la corrección
        // del `while(1)` original: un sensor caído no puede tumbar los otros.
        canal(contrato::Canal::TEMP_AMB).publicarFalla(ahora);
        canal(contrato::Canal::HUM).publicarFalla(ahora);
      }
      xSemaphoreGive(mtxEstado);
    }

    vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(config::MS_TAREA_AMBIENTE));
  }
}

// ===========================================================================
// Tarea: termopares
// ===========================================================================
// Los dos comparten SCK y SO, así que se leen ALTERNADOS y nunca a la vez: dos
// CS activos simultáneamente ponen a dos chips a manejar la misma línea SO y el
// resultado es basura.
static void tareaTermopares(void*) {
  TickType_t ultimo = xTaskGetTickCount();
  bool turnoPrimero = true;

  for (;;) {
    sensores::Termopar& tp = turnoPrimero ? termo1 : termo2;
    const contrato::Canal id = turnoPrimero ? contrato::Canal::TC1
                                            : contrato::Canal::TC2;
    float c = 0;
    const bool ok = tp.leer(c);
    const uint32_t ahora = millis();

    if (!ok && tp.falla() != sensores::FallaTermopar::DEMASIADO_PRONTO) {
      if (turnoPrimero) fallosTermo1++; else fallosTermo2++;
    }

    if (xSemaphoreTake(mtxEstado, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (ok) {
        canal(id).publicar(c, ahora);
      } else if (tp.falla() != sensores::FallaTermopar::DEMASIADO_PRONTO) {
        // DEMASIADO_PRONTO no es una falla del sensor sino del ritmo de sondeo:
        // marcarla como falla haría parpadear el estado sin motivo real.
        canal(id).publicarFalla(ahora);
      }
      xSemaphoreGive(mtxEstado);
    }

    turnoPrimero = !turnoPrimero;
    // La mitad del periodo por chip: cada uno se lee cada ~520 ms, holgadamente
    // por encima de los 250 ms que exige la conversión.
    vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(config::MS_TAREA_TERMOPAR / 2));
  }
}

// ===========================================================================
// Tarea: consolidación
// ===========================================================================
// Envejece los canales y arma la muestra que la Fase 3 publicará. Vive en el
// núcleo 0 —el de la red— porque su trabajo es breve y no toca hardware lento.
static void tareaAgregado(void*) {
  TickType_t ultimo = xTaskGetTickCount();

  for (;;) {
    const uint32_t ahora = millis();
    contrato::Muestra m;

    if (xSemaphoreTake(mtxEstado, pdMS_TO_TICKS(50)) == pdTRUE) {
      for (uint8_t i = 0; i < contrato::NUM_CANALES; i++) {
        // Envejecer SIEMPRE, aunque no lleguen lecturas: es justo cuando un
        // sensor deja de responder que hay que notarlo. Si solo se evaluara al
        // publicar, un canal muerto se quedaría congelado en OK para siempre.
        canales[i].envejecer(ahora);
        m.valor[i] = canales[i].valorPublicable();
        m.faults |= canales[i].bitSiFalla();
      }
      xSemaphoreGive(mtxEstado);
    }

    // La marca monótona se convierte a época al PUBLICAR, no aquí: así las
    // muestras capturadas antes de sincronizar NTP recuperan su hora real en
    // cuanto la hora se conoce, en vez de perderse o quedar todas apiladas en
    // el instante del envío. Ver red/tiempo.h.
    m.tsMs = reloj.aEpocaMs(ahora);

    if (xSemaphoreTake(mtxMuestra, pdMS_TO_TICKS(20)) == pdTRUE) {
      ultimaMuestra = m;
      xSemaphoreGive(mtxMuestra);
    }

    // Encolar para la Ruta B
    almacen::Pendiente p;
    p.marcaMs = ahora;
    p.faults = m.faults;
    for (uint8_t i = 0; i < contrato::NUM_CANALES; i++) p.valor[i] = m.valor[i];
    p._relleno = 0;

    if (xSemaphoreTake(mtxBufer, pdMS_TO_TICKS(50)) == pdTRUE) {
      anillo.agregar(p);

      // Cuando el anillo se acerca al límite, se vuelca la mitad más antigua a
      // flash. Volcar antes de llenarlo evita perder muestras, y hacerlo por
      // bloques —y no en cada muestra— alarga la vida de la memoria.
      if (anillo.tamano() >= anillo.capacidad() - 60 && respaldo.disponible()) {
        static almacen::Pendiente lote[240];
        const size_t n = anillo.asomar(lote, 240);
        if (respaldo.volcar(lote, n)) anillo.descartarFrente(n);
      }
      xSemaphoreGive(mtxBufer);
    }

    vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(config::MS_TAREA_AGREGADO));
  }
}


// ---------------------------------------------------------------------------
// Envío de un lote y ejecución de los comandos que devuelva el servidor
// ---------------------------------------------------------------------------
static void enviarLotePendiente(uint32_t& esperaLoteMs) {
  static almacen::Pendiente lote[60];
  size_t n = 0;
  bool desdeFlash = false;

  // El respaldo en flash se drena PRIMERO: es lo más antiguo, y enviarlo en
  // orden mantiene el histórico coherente.
  if (xSemaphoreTake(mtxBufer, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (respaldo.disponible() && respaldo.registros() > 0) {
      n = respaldo.leerDesde(0, lote, 60);
      desdeFlash = true;
    } else {
      n = anillo.asomar(lote, 60);
    }
    xSemaphoreGive(mtxBufer);
  }
  if (n == 0) return;

  red::Salud salud{gestorWifi.rssi(), millis() / 1000UL,
                   ESP.getFreeHeap(), gestorWifi.reconexiones()};

  red::ComandoPendiente comandos[5];
  size_t numComandos = 0;

  size_t nAcuses = 0;
  static red::AcuseComando copiaAcuses[8];
  if (xSemaphoreTake(mtxAcuses, pdMS_TO_TICKS(20)) == pdTRUE) {
    nAcuses = numAcuses;
    for (size_t i = 0; i < nAcuses; i++) copiaAcuses[i] = acuses[i];
    xSemaphoreGive(mtxAcuses);
  }

  const int r = publicador.enviarLote(lote, n, salud, config::VERSION_FIRMWARE,
                                      copiaAcuses, nAcuses,
                                      comandos, 5, numComandos);

  if (r == -2) {
    // Token inválido o equipo desactivado. Reintentar sería un bucle infinito
    // sin ninguna posibilidad de éxito: requiere intervención humana.
    credencialesInvalidas = true;
    Serial.println("[red] CREDENCIALES RECHAZADAS. Se detiene el envío. "
                   "Revisa el token de ingesta del equipo.");
    return;
  }

  if (r < 0) {
    // Backoff: hasta 5 min extra entre intentos. NO se descarta nada.
    esperaLoteMs = esperaLoteMs == 0 ? 15000 : esperaLoteMs * 2;
    if (esperaLoteMs > 300000) esperaLoteMs = 300000;
    return;
  }

  esperaLoteMs = 0;

  // Un lote aceptado demuestra que este firmware conecta, valida TLS y publica.
  // Solo entonces se fija la imagen: confirmarla antes anularía la reversión
  // automática, que es la red de seguridad ante una versión que se cuelga a los
  // pocos segundos de arrancar.
  red::Ota::confirmarArranque();

  // Solo se descarta TRAS confirmar la entrega. Descartar antes es la forma
  // clásica de perder datos cuando la respuesta se pierde en un timeout.
  if (xSemaphoreTake(mtxBufer, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (desdeFlash) {
      // LittleFS no permite recortar por delante de forma eficiente. Si quedan
      // más registros, se dejan y se drenarán en los siguientes lotes; el
      // archivo se borra entero cuando ya no queda nada por enviar.
      if (respaldo.registros() <= n) respaldo.limpiar();
    } else {
      anillo.descartarFrente(n);
    }
    xSemaphoreGive(mtxBufer);
  }

  // Acuses entregados: se limpian.
  if (nAcuses > 0 && xSemaphoreTake(mtxAcuses, pdMS_TO_TICKS(20)) == pdTRUE) {
    numAcuses = 0;
    xSemaphoreGive(mtxAcuses);
  }

  // --- Ejecución de comandos ------------------------------------------------
  for (size_t i = 0; i < numComandos; i++) {
    ejecutarComando(comandos[i]);
  }
}

/** Ejecuta un comando del servidor y deja su acuse para el siguiente lote. */
static void ejecutarComando(const red::ComandoPendiente& c) {
  red::AcuseComando a{};
  a.id = c.id;

  switch (c.tipo) {
    case contrato::Comando::TARA:
      // La tara se delega a la tarea de báscula, que es la dueña del driver.
      // El acuse se resuelve allí, cuando de verdad se sabe si funcionó.
      solicitudTara = true;
      snprintf(a.detalle, sizeof(a.detalle), "tara encolada");
      a.ok = true;
      break;

    case contrato::Comando::CALIBRAR:
      solicitudCalibrar = c.parametro;
      snprintf(a.detalle, sizeof(a.detalle), "calibracion con %.1f g encolada",
               c.parametro);
      a.ok = true;
      break;

    case contrato::Comando::RECARGAR_UMBRALES:
      snprintf(a.detalle, sizeof(a.detalle), "sin umbrales locales configurados");
      a.ok = true;
      break;

    case contrato::Comando::REINICIAR:
      Serial.println("[red] reinicio solicitado desde la nube");
      delay(200);
      ESP.restart();
      return;

    default:
      snprintf(a.detalle, sizeof(a.detalle), "comando no reconocido");
      a.ok = false;
      break;
  }

  if (xSemaphoreTake(mtxAcuses, pdMS_TO_TICKS(20)) == pdTRUE) {
    if (numAcuses < 8) acuses[numAcuses++] = a;
    xSemaphoreGive(mtxAcuses);
  }
}

// ===========================================================================
// Tarea: red
// ===========================================================================
// Serializa las dos rutas sobre UNA sola conexión TLS. Ver red/publicador.h
// para por qué no pueden ser dos: el saludo TLS cuesta 1–2 s y ~45 KB de heap,
// así que rehacerlo cada 2 s es imposible y mantener dos sockets abiertos deja
// al equipo al borde de quedarse sin memoria.
static void tareaRed(void*) {
  esp_task_wdt_add(nullptr);

  uint32_t ultimoVivoMs = 0;
  uint32_t ultimoLoteMs = 0;
  uint32_t esperaLoteMs = 0;     // backoff tras un fallo

  for (;;) {
    esp_task_wdt_reset();
    gestorWifi.mantener();

    if (gestorWifi.conectado()) {
      reloj.actualizar();
      reloj.resincronizarSiToca();
    }

    const uint32_t ahora = millis();

    // --- Ruta A: tiempo real ------------------------------------------------
    if (gestorWifi.conectado() && ahora - ultimoVivoMs >= 2000) {
      ultimoVivoMs = ahora;
      contrato::Muestra m;
      if (xSemaphoreTake(mtxMuestra, pdMS_TO_TICKS(20)) == pdTRUE) {
        m = ultimaMuestra;
        xSemaphoreGive(mtxMuestra);
      }
      // Efímero por diseño: si falla no se reintenta ni se guarda. El histórico
      // va por la Ruta B, así que perderlo solo salta un refresco del panel.
      if (m.tsMs > 0) publicador.transmitirEnVivo(m);
    }

    // --- Ruta B: lote -------------------------------------------------------
    if (gestorWifi.conectado() && !credencialesInvalidas &&
        ahora - ultimoLoteMs >= (30000 + esperaLoteMs)) {
      ultimoLoteMs = ahora;
      enviarLotePendiente(esperaLoteMs);
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}


// ===========================================================================
// Aprovisionamiento por puerto serie
// ===========================================================================
// Alternativa al portal cautivo. Existe por una razón práctica: el JWT del
// equipo son ~256 caracteres y el token ~43, y teclear eso en el teclado de un
// celular es impracticable. Por USB se pegan con copiar-pegar.
//
// Los valores van a NVS igual que por el portal; NADA de esto toca el código.
static void aplicarSet(String resto) {
  resto.trim();
  const int esp = resto.indexOf(' ');
  if (esp <= 0) {
    Serial.println("[config] uso: set <ssid|pass|url|anon|slug|token|jwt> <valor>");
    return;
  }
  const String campo = resto.substring(0, esp);
  String valor = resto.substring(esp + 1);
  valor.trim();

  red::Credenciales c = almacenCred.leer();

  if (campo == "ssid") {
    gestorWifi.fijarRed(valor, prefsPassTemporal);
    Serial.printf("[config] ssid = %s\n", valor.c_str());
  } else if (campo == "pass") {
    prefsPassTemporal = valor;
    gestorWifi.fijarRed(gestorWifi.ssidGuardado(), valor);
    Serial.println("[config] contraseña WiFi guardada");
  } else if (campo == "url")   { c.urlSupabase = valor;    almacenCred.guardar(c);
    Serial.printf("[config] url = %s\n", valor.c_str());
  } else if (campo == "anon")  { c.anonKey = valor;        almacenCred.guardar(c);
    Serial.printf("[config] anon key guardada (%u chars)\n", valor.length());
  } else if (campo == "slug")  { c.slugEquipo = valor;     almacenCred.guardar(c);
    Serial.printf("[config] slug = %s\n", valor.c_str());
  } else if (campo == "token") { c.tokenIngesta = valor;   almacenCred.guardar(c);
    Serial.printf("[config] token guardado (%u chars)\n", valor.length());
  } else if (campo == "jwt")   { c.jwtDispositivo = valor; almacenCred.guardar(c);
    Serial.printf("[config] jwt guardado (%u chars)\n", valor.length());
  } else {
    Serial.printf("[config] campo desconocido: %s\n", campo.c_str());
    return;
  }

  // Si ya está todo, se avisa. No se reinicia solo: quien configura decide
  // cuándo, y así puede revisar con `ver` antes de arrancar.
  c = almacenCred.leer();
  if (c.completas() && gestorWifi.ssidGuardado().length()) {
    Serial.println("[config] configuración COMPLETA · escribe 'r' para reiniciar y conectar");
  }
}

/** Muestra la configuración con los secretos enmascarados. */
static void imprimirConfig() {
  const red::Credenciales c = almacenCred.leer();
  auto mascara = [](const String& v) -> String {
    if (v.length() == 0) return "(vacío)";
    if (v.length() <= 8) return "********";
    return v.substring(0, 4) + "…" + v.substring(v.length() - 4) +
           " (" + String(v.length()) + " chars)";
  };
  Serial.println();
  Serial.println("──────────── configuración ────────────");
  Serial.printf("  ssid   %s\n", gestorWifi.ssidGuardado().c_str());
  Serial.printf("  url    %s\n", c.urlSupabase.c_str());
  Serial.printf("  slug   %s\n", c.slugEquipo.c_str());
  Serial.printf("  anon   %s\n", mascara(c.anonKey).c_str());
  Serial.printf("  token  %s\n", mascara(c.tokenIngesta).c_str());
  Serial.printf("  jwt    %s\n", mascara(c.jwtDispositivo).c_str());
  Serial.printf("  estado %s · tiempo real %s\n",
                c.completas() ? "COMPLETA" : "INCOMPLETA",
                c.puedeTiempoReal() ? "disponible" : "no configurado");
  Serial.println("───────────────────────────────────────");
  Serial.println();
}

/** Atiende el puerto serie mientras el portal cautivo está abierto. */
static void atenderSerieEnPortal() {
  static String linea;
  while (Serial.available()) {
    const char ch = Serial.read();
    if (ch == '\n' || ch == '\r') {
      linea.trim();
      if (linea.length()) {
        if (linea.startsWith("set ")) {
          aplicarSet(linea.substring(4));
          const red::Credenciales c = almacenCred.leer();
          if (c.completas() && gestorWifi.ssidGuardado().length()) {
            gestorWifi.marcarGuardadoPorSerie();
          }
        } else if (linea == "ver") {
          imprimirConfig();
        } else if (linea == "r" || linea == "reset") {
          ESP.restart();
        } else {
          Serial.println("[portal] set <ssid|pass|url|anon|slug|token|jwt> <valor> · ver · r");
        }
      }
      linea = "";
    } else if (linea.length() < 512) {
      linea += ch;
    }
  }
}

// ===========================================================================
// Tarea: consola
// ===========================================================================
// Sustituye a serialEvent(), que NO existe en el core ESP32 de Arduino: en el
// prototipo esa función nunca llegó a ejecutarse, así que la calibración por
// teclado jamás funcionó aunque el código estuviera escrito.
static void tareaConsola(void*) {
  TickType_t ultimo = xTaskGetTickCount();
  String linea;

  for (;;) {
    while (Serial.available()) {
      const char c = Serial.read();
      if (c == '\n' || c == '\r') {
        linea.trim();
        if (linea.length()) {
          if (linea == "t" || linea == "tara") {
            solicitudTara = true;
            Serial.println("[consola] tara solicitada");
          } else if (linea.startsWith("cal ")) {
            const float p = linea.substring(4).toFloat();
            if (p > 0) {
              solicitudCalibrar = p;
              Serial.printf("[consola] calibración con %.1f g solicitada\n", p);
            } else {
              Serial.println("[consola] uso: cal <gramos>");
            }
          } else if (linea == "d" || linea == "diag") {
            imprimirDiagnostico();
          } else if (linea.startsWith("set ")) {
            aplicarSet(linea.substring(4));
          } else if (linea == "ver") {
            imprimirConfig();
          } else if (linea == "olvidar") {
            gestorWifi.olvidar();
            Serial.println("[consola] credenciales borradas · reiniciando");
            delay(200);
            ESP.restart();
          } else if (linea == "r" || linea == "reset") {
            Serial.println("[consola] reiniciando…");
            delay(100);
            ESP.restart();
          } else {
            Serial.println("[consola] t (tara) · cal <g> · d (diag) · ver · "
                           "set <campo> <valor> · olvidar · r (reset)");
          }
        }
        linea = "";
      } else if (linea.length() < 512) {   // el JWT del equipo son ~256 chars
        linea += c;
      }
    }
    vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(50));
  }
}

// ===========================================================================
// Diagnóstico
// ===========================================================================
static void imprimirDiagnostico() {
  Serial.println();
  Serial.println("──────────────── diagnóstico ────────────────");
  Serial.printf("firmware      %s\n", config::VERSION_FIRMWARE);
  Serial.printf("arranque núm. %u\n", numArranque);
  Serial.printf("activo        %lu s\n", millis() / 1000UL);
  Serial.printf("heap libre    %u bytes (mínimo histórico %u)\n",
                ESP.getFreeHeap(), ESP.getMinFreeHeap());
  Serial.printf("báscula       %u lecturas · offset=%d factor=%.3f · %s\n",
                lecturasBascula,
                bascula.calibracion().offset(),
                bascula.calibracion().factor(),
                bascula.estable() ? "estable" : "inestable");
  Serial.printf("SHT31         %s · %u inicializaciones\n",
                ambiente.disponible() ? "presente" : "AUSENTE",
                ambiente.reinicios());
  Serial.printf("termopar 1    %s · %u fallos\n",
                sensores::nombre(termo1.falla()), fallosTermo1);
  Serial.printf("termopar 2    %s · %u fallos\n",
                sensores::nombre(termo2.falla()), fallosTermo2);

  if (xSemaphoreTake(mtxEstado, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (uint8_t i = 0; i < contrato::NUM_CANALES; i++) {
      const auto c = static_cast<contrato::Canal>(i);
      Serial.printf("  %-9s %-12s crudo=%.2f\n",
                    contrato::slug(c),
                    sensores::nombre(canales[i].estado()),
                    canales[i].valorCrudo());
    }
    xSemaphoreGive(mtxEstado);
  }
  Serial.println("─────────────────────────────────────────────");
  Serial.println();
}

/** Línea compacta periódica, para ver el proceso en vivo por consola. */
static void tareaTraza(void*) {
  TickType_t ultimo = xTaskGetTickCount();

  for (;;) {
    contrato::Muestra m;
    if (xSemaphoreTake(mtxMuestra, pdMS_TO_TICKS(20)) == pdTRUE) {
      m = ultimaMuestra;
      xSemaphoreGive(mtxMuestra);
    }

    Serial.print("[");
    for (uint8_t i = 0; i < contrato::NUM_CANALES; i++) {
      const auto c = static_cast<contrato::Canal>(i);
      Serial.printf("%s=", contrato::slug(c));
      if (isnan(m.valor[i])) Serial.print("—");
      else Serial.printf("%.1f", m.valor[i]);
      if (i + 1 < contrato::NUM_CANALES) Serial.print(" ");
    }
    Serial.printf("] faults=0x%02X heap=%u\n", m.faults, ESP.getFreeHeap());

    vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(config::MS_DIAGNOSTICO));
  }
}


// ---------------------------------------------------------------------------
// Página local de servicio
// ---------------------------------------------------------------------------
static red::EstadoServicio estadoServicio;

static red::EstadoServicio* obtenerEstadoServicio() {
  estadoServicio.fw = config::VERSION_FIRMWARE;
  estadoServicio.wifiConectado = gestorWifi.conectado();
  estadoServicio.rssi = gestorWifi.rssi();
  estadoServicio.horaSincronizada = reloj.sincronizado();
  estadoServicio.uptimeS = millis() / 1000UL;
  estadoServicio.heapLibre = ESP.getFreeHeap();
  estadoServicio.lotesOk = publicador.lotesOk();
  estadoServicio.lotesFallo = publicador.lotesFallo();
  estadoServicio.vivosOk = publicador.vivosOk();
  estadoServicio.vivosFallo = publicador.vivosFallo();
  estadoServicio.peso = bascula.gramos();
  estadoServicio.bascEstable = bascula.estable();
  estadoServicio.offset = bascula.calibracion().offset();
  estadoServicio.factor = bascula.calibracion().factor();

  if (xSemaphoreTake(mtxBufer, pdMS_TO_TICKS(20)) == pdTRUE) {
    estadoServicio.pendientesRam = anillo.tamano();
    estadoServicio.pendientesFlash = respaldo.registros();
    xSemaphoreGive(mtxBufer);
  }

  if (xSemaphoreTake(mtxEstado, pdMS_TO_TICKS(20)) == pdTRUE) {
    for (uint8_t i = 0; i < contrato::NUM_CANALES; i++) {
      estadoServicio.estadoCanal[i] = sensores::nombre(canales[i].estado());
      estadoServicio.valorCanal[i] = canales[i].valorCrudo();
    }
    xSemaphoreGive(mtxEstado);
  }
  return &estadoServicio;
}

static void tareaLocal(void*) {
  for (;;) {
    servidorLocal.atender();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ===========================================================================
// Arranque
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.printf("=== Monitoreo ESP32 · %s ===\n", config::VERSION_FIRMWARE);

  ajustes.iniciar();
  numArranque = ajustes.registrarArranque();

  mtxEstado = xSemaphoreCreateMutex();
  mtxMuestra = xSemaphoreCreateMutex();

  // --- Canales ------------------------------------------------------------
  canal(contrato::Canal::PESO).configurar(
      contrato::Canal::PESO,
      {config::PESO_MIN_G, config::PESO_MAX_G, config::MS_OBSOLETO_BASCULA});
  canal(contrato::Canal::TEMP_AMB).configurar(
      contrato::Canal::TEMP_AMB,
      {config::TEMP_AMB_MIN_C, config::TEMP_AMB_MAX_C, config::MS_OBSOLETO_AMBIENTE});
  canal(contrato::Canal::HUM).configurar(
      contrato::Canal::HUM,
      {config::HUM_MIN_PCT, config::HUM_MAX_PCT, config::MS_OBSOLETO_AMBIENTE});
  canal(contrato::Canal::TC1).configurar(
      contrato::Canal::TC1,
      {config::TC_MIN_C, config::TC_MAX_C, config::MS_OBSOLETO_TERMOPAR});
  canal(contrato::Canal::TC2).configurar(
      contrato::Canal::TC2,
      {config::TC_MIN_C, config::TC_MAX_C, config::MS_OBSOLETO_TERMOPAR});

  // --- Báscula ------------------------------------------------------------
  bascula.iniciar(config::PIN_HX711_DT, config::PIN_HX711_SCK);
  const almacen::AjustesBascula ab = ajustes.leerBascula();
  bascula.calibracion().fijarOffset(ab.offset);
  bascula.calibracion().fijarFactor(ab.factor);
  Serial.printf("[bascula] offset=%d factor=%.3f (%u taras previas)\n",
                ab.offset, ab.factor, ab.tarasRealizadas);

  // --- Ambiente -----------------------------------------------------------
  // Si falla, se avisa y se sigue. Reintentará solo cada 5 s.
  if (ambiente.iniciar(config::PIN_I2C_SDA, config::PIN_I2C_SCL)) {
    Serial.println("[sht31] presente");
  } else {
    Serial.println("[sht31] AUSENTE — el equipo sigue operando con los demás "
                   "canales y reintentará cada 5 s");
  }

  // --- Termopares ---------------------------------------------------------
  termo1.iniciar(config::PIN_TERMO_SCK, config::PIN_TERMO_CS1, config::PIN_TERMO_SO);
  termo2.iniciar(config::PIN_TERMO_SCK, config::PIN_TERMO_CS2, config::PIN_TERMO_SO);
  delay(300);   // el MAX6675 necesita ~200 ms tras energizar
  Serial.println("[max6675] listos");

  // --- Red ----------------------------------------------------------------
  mtxBufer = xSemaphoreCreateMutex();
  mtxAcuses = xSemaphoreCreateMutex();

  almacenCred.iniciar();
  credenciales = almacenCred.leer();
  gestorWifi.iniciar(&almacenCred);

  if (respaldo.iniciar()) {
    Serial.printf("[flash] respaldo montado · %u muestras pendientes\n",
                  respaldo.registros());
  } else {
    Serial.println("[flash] respaldo NO disponible: solo búfer en RAM");
  }

  if (!credenciales.completas()) {
    // Sin configurar no hay nada útil que hacer, así que el portal bloquea a
    // propósito. Los sensores no se inician: se reiniciará al guardar.
    Serial.println("[red] equipo SIN CONFIGURAR");
    Serial.println("      Opción A: conéctate al WiFi del equipo y llena el formulario.");
    Serial.println("      Opción B (recomendada): pega aquí los valores. El JWT son");
    Serial.println("      ~256 caracteres y teclearlo en un celular es impracticable.");
    Serial.println("        set ssid  <nombre de tu red>");
    Serial.println("        set pass  <contraseña>");
    Serial.println("        set url   https://xxxx.supabase.co");
    Serial.println("        set slug  planta-01");
    Serial.println("        set token <token de ingesta>");
    Serial.println("        set jwt   <jwt del equipo>");
    Serial.println("        set anon  <anon key>");
    Serial.println("        ver   para revisar · r para reiniciar y conectar\n");
    gestorWifi.portalCautivo(atenderSerieEnPortal);
    // Al completarse por serie el bucle termina; se reinicia para arrancar limpio.
    Serial.println("[red] configuración completa · reiniciando");
    delay(500);
    ESP.restart();
  }

  if (gestorWifi.conectar()) {
    Serial.printf("[wifi] conectado · IP %s · %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("[wifi] sin conexión al arrancar; se reintentará en segundo plano");
  }

  reloj.iniciar();
  publicador.iniciar(credenciales, &reloj);

  servidorLocal.iniciar(obtenerEstadoServicio,
                        []() { solicitudTara = true; },
                        [](float p) { solicitudCalibrar = p; });
  Serial.println("[local] página de servicio en http://monitoreo.local");

  // Watchdog: si una tarea se cuelga, el equipo se reinicia solo. 30 s cubre
  // con holgura el peor caso —un saludo TLS lento sobre una red saturada— sin
  // dejar al equipo colgado durante minutos.
  esp_task_wdt_init(30, true);

  // --- Tareas -------------------------------------------------------------
  // Núcleo 1 para el hardware lento; núcleo 0 para la red.
  xTaskCreatePinnedToCore(tareaBascula,    "bascula",   4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(tareaAmbiente,   "ambiente",  4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(tareaTermopares, "termopar",  4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(tareaAgregado,   "agregado",  4096, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(tareaConsola,    "consola",   4096, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(tareaTraza,      "traza",     4096, nullptr, 1, nullptr, 0);
  // Pila mayor: TLS y JSON necesitan bastante espacio de pila.
  xTaskCreatePinnedToCore(tareaRed,        "red",      12288, nullptr, 4, nullptr, 0);
  xTaskCreatePinnedToCore(tareaLocal,      "local",     4096, nullptr, 1, nullptr, 0);

  Serial.println("[sistema] tareas iniciadas · escribe 'd' para diagnóstico\n");
}

// El trabajo vive en las tareas. loop() corre en la tarea Arduino del núcleo 1
// y solo cede tiempo; poner trabajo aquí reintroduciría el bloqueo original.
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
