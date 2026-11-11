#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <string.h>

// ====== ЖЕЛЕЗО ======
#define PUMP_PIN 2   // Пин насоса (проверь по своей плате)

// ====== UUID (ДОЛЖНЫ СОВПАДАТЬ С ANDROID) ======
#define SERVICE_UUID        "4faa1a00-1234-4567-89ab-cdef12345678"
#define CHAR_STATUS_UUID    "4faa1a01-1234-4567-89ab-cdef12345678"  // R/N: статус (MOVING/IDLE)
#define CHAR_CONTROL_UUID   "4faa1a02-1234-4567-89ab-cdef12345678"  // W: команды (1/0/M:...)

// ====== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ======
BLEServer*         pServer       = nullptr;
BLECharacteristic* pCharStatus   = nullptr;
BLECharacteristic* pCharControl  = nullptr;
BLEAdvertising*    pAdvertising  = nullptr;

bool deviceConnected = false;
bool pumpOn          = false;

// Режимы
enum Mode {
  MODE_OFF,
  MODE_LIGHT,
  MODE_MEDIUM,
  MODE_HARD
};

Mode currentMode = MODE_OFF;

// Параметры режимов (секунды и минуты)
float lightPulse  = 0.5f;
int   lightInt    = 10;
float medPulse    = 1.0f;
int   medInt      = 5;
float hardPulse   = 2.0f;
int   hardInt     = 2;

// Текущее активное значение (выбранный режим)
float    activePulseSec = 0.0f;
uint32_t activeIntMs    = 0;

// Логика движения (телефон решает, ESP32 только знает «движемся/нет»)
bool movingActive = false;

// Таймеры автокапель
bool     autoCycleRunning = false;
uint32_t nextDropTime     = 0;
uint32_t pumpOffTime      = 0;

// ====== ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ ======
void setPump(bool on) {
  pumpOn = on;
  digitalWrite(PUMP_PIN, on ? HIGH : LOW);
  Serial.print("PUMP: ");
  Serial.println(on ? "ON" : "OFF");
}

void updateActiveParams() {
  switch (currentMode) {
    case MODE_LIGHT:
      activePulseSec = lightPulse;
      activeIntMs    = (uint32_t)lightInt * 60000UL;
      break;
    case MODE_MEDIUM:
      activePulseSec = medPulse;
      activeIntMs    = (uint32_t)medInt * 60000UL;
      break;
    case MODE_HARD:
      activePulseSec = hardPulse;
      activeIntMs    = (uint32_t)hardInt * 60000UL;
      break;
    case MODE_OFF:
    default:
      activePulseSec = 0.0f;
      activeIntMs    = 0;
      break;
  }

  Serial.print("Active mode: ");
  switch (currentMode) {
    case MODE_LIGHT:  Serial.print("LIGHT"); break;
    case MODE_MEDIUM: Serial.print("MEDIUM"); break;
    case MODE_HARD:   Serial.print("HARD"); break;
    case MODE_OFF:    Serial.print("OFF"); break;
  }
  Serial.print("  pulse=");
  Serial.print(activePulseSec);
  Serial.print("s  interval=");
  Serial.print(activeIntMs / 60000);
  Serial.println("m");
}

void resetAutoCycle() {
  autoCycleRunning = false;
  nextDropTime     = 0;
  pumpOffTime      = 0;
}

// ====== CALLBACK ДЛЯ КОМАНД ИЗ ANDROID ======
class ControlCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* pCharacteristic) override {
    std::string value = pCharacteristic->getValue();
    if (value.empty()) return;

    const char* cmd = value.c_str();
    Serial.print("CMD: ");
    Serial.println(cmd);

    // Прямое управление насосом
    if (strcmp(cmd, "1") == 0 || strcmp(cmd, "PUMP_START") == 0) {
      setPump(true);
      return;
    }
    if (strcmp(cmd, "0") == 0 || strcmp(cmd, "PUMP_STOP") == 0) {
      setPump(false);
      return;
    }

    // Команды движения от телефона
    if (strcmp(cmd, "START") == 0) {
      movingActive = true;
      Serial.println("MOVING: TRUE (from phone)");
      if (currentMode != MODE_OFF && activePulseSec > 0.0f && activeIntMs > 0) {
        autoCycleRunning = true;
        nextDropTime     = millis();  // первую каплю можно дать сразу
        pumpOffTime      = 0;
      }
      return;
    }

    if (strcmp(cmd, "STOP") == 0) {
      movingActive = false;
      Serial.println("MOVING: FALSE (from phone)");
      resetAutoCycle();
      setPump(false);
      return;
    }

    // Режимы: M:LIGHT:0.5:10
    if (cmd[0] == 'M' && cmd[1] == ':') {
      char buf[64];
      strncpy(buf, cmd, sizeof(buf) - 1);
      buf[sizeof(buf) - 1] = '\0';

      char* token = strtok(buf, ":"); // "M"
      token = strtok(nullptr, ":");   // MODE
      if (!token) return;
      const char* mode = token;

      token = strtok(nullptr, ":");   // PULSE
      if (!token) return;
      float pulse = atof(token);

      token = strtok(nullptr, ":");   // INTERVAL
      if (!token) return;
      int interval = atoi(token);

      Serial.print("MODE CMD: ");
      Serial.print(mode);
      Serial.print("  PULSE: ");
      Serial.print(pulse);
      Serial.print("  INT: ");
      Serial.println(interval);

      if (strcmp(mode, "LIGHT") == 0) {
        lightPulse = pulse;
        lightInt   = interval;
        currentMode = MODE_LIGHT;
      } else if (strcmp(mode, "MEDIUM") == 0) {
        medPulse = pulse;
        medInt   = interval;
        currentMode = MODE_MEDIUM;
      } else if (strcmp(mode, "HARD") == 0) {
        hardPulse = pulse;
        hardInt   = interval;
        currentMode = MODE_HARD;
      } else if (strcmp(mode, "OFF") == 0) {
        currentMode = MODE_OFF;
      }

      updateActiveParams();

      if (currentMode == MODE_OFF) {
        resetAutoCycle();
        setPump(false);
      } else {
        if (movingActive && activePulseSec > 0.0f && activeIntMs > 0) {
          autoCycleRunning = true;
          nextDropTime     = millis();
          pumpOffTime      = 0;
        }
      }
    }
  }
};

// ====== CALLBACK ДЛЯ ПОДКЛЮЧЕНИЯ/ОТКЛЮЧЕНИЯ ======
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    deviceConnected = true;
    Serial.println("Client connected");
  }

  void onDisconnect(BLEServer* server) override {
    deviceConnected = false;
    Serial.println("Client disconnected");
    if (pAdvertising) {
      pAdvertising->start();
      Serial.println("Advertising restarted");
    }
  }
};

// ====== SETUP ======
void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PUMP_PIN, OUTPUT);
  setPump(false);

  Serial.println("Init BLE...");
  BLEDevice::init("OILER_V26");

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  // Статус (READ + NOTIFY)
  pCharStatus = pService->createCharacteristic(
    CHAR_STATUS_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharStatus->addDescriptor(new BLE2902());

  // Управление (WRITE / WRITE_NR)
  pCharControl = pService->createCharacteristic(
    CHAR_CONTROL_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCharControl->setCallbacks(new ControlCallbacks());

  pService->start();

  // Advertising для ESP32 Arduino Core 3.x
  pAdvertising = pServer->getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->start();
  Serial.println("BLE ready, advertising OILER_V26");

  updateActiveParams();
}

// ====== LOOP ======
void loop() {
  static uint32_t lastStatusUpdate = 0;
  static uint32_t lastSeen         = 0;

  uint32_t now = millis();

  if (deviceConnected) {
    lastSeen = now;

    // Статус раз в секунду
    if (now - lastStatusUpdate > 1000) {
      lastStatusUpdate = now;
      const char* status = movingActive ? "MOVING" : "IDLE";
      pCharStatus->setValue(status);
      pCharStatus->notify();
    }

    // Логика автокапель
    if (movingActive && currentMode != MODE_OFF && activePulseSec > 0.0f && activeIntMs > 0) {
      if (!autoCycleRunning) {
        autoCycleRunning = true;
        nextDropTime     = now;
        pumpOffTime      = 0;
      }

      if (pumpOn) {
        if (now >= pumpOffTime) {
          setPump(false);
          nextDropTime = now + activeIntMs;
        }
      } else {
        if (now >= nextDropTime) {
          setPump(true);
          pumpOffTime = now + (uint32_t)(activePulseSec * 1000.0f);
        }
      }
    } else {
      if (autoCycleRunning) {
        autoCycleRunning = false;
        setPump(false);
      }
    }

  } else {
    if (now - lastSeen > 5000 && pAdvertising) {
      pAdvertising->start();
    }
  }

  delay(50);
}
