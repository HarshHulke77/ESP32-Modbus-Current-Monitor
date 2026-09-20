#include <WiFi.h>          // only used to explicitly disable WiFi (saves power, avoids interference)
#include <ModbusRTU.h>
#include <Preferences.h>

Preferences prefs;

const float SANITY_WINDOW_V = 0.15;   // reject fresh average if it's further than this from lastGood
const float NVS_WRITE_THRESHOLD_V = 0.015;  // only rewrite NVS if drift exceeds this

// ---------- Pin definitions ----------
#define ACS712_PIN     34      // ADC1_CH6, input-only pin — safe for analog read
#define MAX485_DE_RE   4       // DE and RE tied together on this single GPIO
#define MODBUS_RX_PIN  26      // ESP32 RX  <- MAX485 RO
#define MODBUS_TX_PIN  27      // ESP32 TX  -> MAX485 DI

// ---------- Modbus config ----------
#define MODBUS_SLAVE_ID  1
#define MODBUS_BAUD      9600
#define REG_CURRENT_MA   0     // holding register index for current reading

// ---------- ACS712 calibration ----------
float ZERO_CURRENT_VOLTAGE = 1.7952;  // fallback of last resort; overwritten in calibrateZero()
const float SENSITIVITY_V_PER_A = 0.1272;    // V per A, after 10k/22k divider (0.185 × 22/32) — UNVALIDATED, see Bug Journal 003
const float ADC_VREF             = 3.30;
const int   ADC_RESOLUTION       = 4095;
const int   SAMPLES_PER_READING  = 50;

// ---------- Shared state ----------
volatile uint16_t g_currentMilliamps = 0;
SemaphoreHandle_t g_dataMutex;

ModbusRTU mb;

// ============================================================
// Task 1: Sample the ACS712 and update the shared current value
// ============================================================
void TaskReadCurrent(void *pvParameters) {
  for (;;) {
    long sum = 0;
    for (int i = 0; i < SAMPLES_PER_READING; i++) {
      sum += analogReadMilliVolts(ACS712_PIN);
      delayMicroseconds(200);
    }
    float avgRaw = (float)sum / SAMPLES_PER_READING;
    float voltage = avgRaw / 1000.0;

    float current_A = (voltage - ZERO_CURRENT_VOLTAGE) / SENSITIVITY_V_PER_A;
    int16_t current_mA = (int16_t)(current_A * 1000.0);


    Serial.printf("Raw ADC avg: %.1f | Voltage: %.3fV | Current: %.1f mA\n",
                  avgRaw, voltage, (float)current_mA);

    if (xSemaphoreTake(g_dataMutex, portMAX_DELAY) == pdTRUE) {
      g_currentMilliamps = current_mA;
      xSemaphoreGive(g_dataMutex);
    }

    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ============================================================
// Task 2: Service Modbus RTU requests and keep the register
//          synced with the latest sampled current value
// ============================================================
void TaskModbusService(void *pvParameters) {
  for (;;) {
    uint16_t latest;
    if (xSemaphoreTake(g_dataMutex, portMAX_DELAY) == pdTRUE) {
      latest = g_currentMilliamps;
      xSemaphoreGive(g_dataMutex);
    }
   mb.Hreg(REG_CURRENT_MA, (uint16_t)latest);
    // --- DEBUG: confirm whether anything is arriving on Serial2 RX at all ---
    if (Serial2.available()) {
      Serial.println("Bytes arrived on Serial2!");
    }

    mb.task();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void calibrateZero() {
  // Step 1: read last-known-good from flash (1.7952 default if never saved)
  prefs.begin("acs712", false);
  float lastGood = prefs.getFloat("zeroV", 1.7952);

  // Step 2: fresh 500-sample average
  long sum = 0;
  for (int i = 0; i < 500; i++) {
    sum += analogReadMilliVolts(ACS712_PIN);
    delayMicroseconds(200);
  }
  float freshAvg = (sum / 500.0) / 1000.0;  // convert mV sum to average V

  // Step 3: sanity check against lastGood
  if (fabs(freshAvg - lastGood) <= SANITY_WINDOW_V) {
    ZERO_CURRENT_VOLTAGE = freshAvg;
    Serial.printf("Calibration: fresh average %.4fV accepted.\n", freshAvg);

    if (fabs(freshAvg - lastGood) > NVS_WRITE_THRESHOLD_V) {
      prefs.putFloat("zeroV", freshAvg);
      Serial.println("Calibration: drift exceeded threshold, NVS updated.");
    }
  } else {
    ZERO_CURRENT_VOLTAGE = lastGood;
    Serial.printf("Calibration: fresh average %.4fV REJECTED (lastGood=%.4fV). Using lastGood.\n",
                  freshAvg, lastGood);
  }

  prefs.end();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n--- Project 2: Industrial Current Monitor (Modbus RTU) ---");
  WiFi.mode(WIFI_OFF);
  analogReadResolution(12);
  analogSetPinAttenuation(ACS712_PIN, ADC_11db);
  
  g_dataMutex = xSemaphoreCreateMutex();
  Serial2.begin(MODBUS_BAUD, SERIAL_8N1, MODBUS_RX_PIN, MODBUS_TX_PIN);
  mb.begin(&Serial2, MAX485_DE_RE);
  mb.slave(MODBUS_SLAVE_ID);
  mb.addHreg(REG_CURRENT_MA, 0);
  calibrateZero();                              // <-- new line, added here
  xTaskCreatePinnedToCore(TaskReadCurrent,   "ReadCurrent",   4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskModbusService, "ModbusService", 4096, NULL, 2, NULL, 1);
  Serial.println("Setup complete. Waiting for Modbus master requests...");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}