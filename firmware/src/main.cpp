// ============================================================================
// Monitoreo ESP32 — Fase 2: adquisición de sensores
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
// El reparto por núcleo prepara la Fase 3: el núcleo 0 queda reservado para la
// pila de red, así que ninguna lectura lenta podrá tumbar la conexión TLS.
// ============================================================================

#include <Arduino.h>

#include "config.h"
#include "modelo/contrato.h"
#include "sensores/canal.h"
#include "sensores/bascula.h"
#include "sensores/ambiente.h"
#include "sensores/termopares.h"
#include "almacen/ajustes.h"

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
static void imprimirDiagnostico();   // usada por la tarea de consola

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

    m.tsMs = 0;   // la Fase 3 pondrá aquí la hora NTP

    if (xSemaphoreTake(mtxMuestra, pdMS_TO_TICKS(20)) == pdTRUE) {
      ultimaMuestra = m;
      xSemaphoreGive(mtxMuestra);
    }

    vTaskDelayUntil(&ultimo, pdMS_TO_TICKS(config::MS_TAREA_AGREGADO));
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
          } else if (linea == "r" || linea == "reset") {
            Serial.println("[consola] reiniciando…");
            delay(100);
            ESP.restart();
          } else {
            Serial.println("[consola] comandos: t (tara) · cal <g> · d (diag) · r (reset)");
          }
        }
        linea = "";
      } else if (linea.length() < 64) {
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

  // --- Tareas -------------------------------------------------------------
  // Núcleo 1 para el hardware lento; núcleo 0 se reserva a la red (Fase 3).
  xTaskCreatePinnedToCore(tareaBascula,    "bascula",   4096, nullptr, 3, nullptr, 1);
  xTaskCreatePinnedToCore(tareaAmbiente,   "ambiente",  4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(tareaTermopares, "termopar",  4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(tareaAgregado,   "agregado",  4096, nullptr, 3, nullptr, 0);
  xTaskCreatePinnedToCore(tareaConsola,    "consola",   4096, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(tareaTraza,      "traza",     4096, nullptr, 1, nullptr, 0);

  Serial.println("[sistema] tareas iniciadas · escribe 'd' para diagnóstico\n");
}

// El trabajo vive en las tareas. loop() corre en la tarea Arduino del núcleo 1
// y solo cede tiempo; poner trabajo aquí reintroduciría el bloqueo original.
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
