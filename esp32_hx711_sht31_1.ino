#include "HX711.h"
#include <Wire.h>
#include "Adafruit_SHT31.h"
#include "max6675.h"   

// Definiciones de pines para ESP32 - Celda de carga (HX711)
#define DOUT 4   // Pin DT
#define CLK 5   // Pin SCK

// Pines I2C para SHT31 (por defecto en ESP32)
#define SDA_PIN 21
#define SCL_PIN 22

// Pines SPI (software) para los MAX6675
#define THERMO_SCK  18   // compartido
#define THERMO_SO   19   // compartido
#define THERMO_CS1  23   // termopar 1
#define THERMO_CS2  25   // termopar 2

HX711 scale;
Adafruit_SHT31 sht31 = Adafruit_SHT31();
MAX6675 termo1(THERMO_SCK, THERMO_CS1, THERMO_SO);
MAX6675 termo2(THERMO_SCK, THERMO_CS2, THERMO_SO);

float calibration_factor = 425;
// Valor inicial, ajustar

void setup() {
  Serial.begin(115200);

  // Inicializar bus I2C para el SHT31
  Wire.begin(SDA_PIN, SCL_PIN);

  // Inicializar HX711
  scale.begin(DOUT, CLK);
  Serial.println("Procedimiento de Calibración HX711");
  scale.set_scale(calibration_factor);
  scale.tare(10);  // Tara con 10 muestras
  Serial.println("Coloque un peso conocido e ingrese el valor en el Monitor Serial");
  Serial.println("Use 'u' para aumentar, 'd' para disminuir el factor");

  // Inicializar SHT31 (dirección I2C por defecto 0x44)
  if (!sht31.begin(0x44)) {
    Serial.println("ERROR: No se encontró el sensor SHT31. Revise el cableado I2C.");
    while (1) delay(10);
  }
  Serial.println("Sensor SHT31 inicializado correctamente");

  // Estabilización de los MAX6675 (~200 ms mínimo tras energizar)
  delay(1000);
  Serial.println("Termopares MAX6675 listos");
}

void loop() {
  // --- Lectura de celda de carga ---
  Serial.print("Lectura: ");
  Serial.print(scale.get_units(10), 1);  // 10 muestras, 1 decimal
  Serial.print(" g");
  Serial.print(" | Factor de Calibración: ");
  Serial.print(calibration_factor);

  // --- Lectura de temperatura y humedad SHT31 ---
  float temp = sht31.readTemperature();
  float hum  = sht31.readHumidity();

  Serial.print(" | Temp: ");
  if (!isnan(temp)) {
    Serial.print(temp, 1);
    Serial.print(" C");
  } else {
    Serial.print("Error");
  }

  Serial.print(" | Humedad: ");
  if (!isnan(hum)) {
    Serial.print(hum, 1);
    Serial.print(" %");
  } else {
    Serial.print("Error");
  }
  // --- Lectura de termopares ---
  float t1 = termo1.readCelsius();
  float t2 = termo2.readCelsius();

  Serial.print(" | T1: ");
  if (!isnan(t1)) { Serial.print(t1, 1); Serial.print(" C"); }
  else Serial.print("Error");

  Serial.print(" | T2: ");
  if (!isnan(t2)) { Serial.print(t2, 1); Serial.print(" C"); }
  else Serial.print("Error");
  
  Serial.println();
  delay(500);
}

// Entrada del Monitor Serial para ajustar calibración
void serialEvent() {
  char inChar = Serial.read();
  if (inChar == 'u') calibration_factor += .5;
  if (inChar == 'd') calibration_factor -= .5;
  if (inChar == 't') scale.set_scale(calibration_factor);
  scale.set_scale(calibration_factor);
}
