/*
  =====================================================================
  РОЗУМНИЙ БУДИНОК НА ESP32-S2 mini (MQTT + RFID + SERVO + ALARMS + PUMP)
  Виправлено: Усунено тремтіння сервоприводу та зациклення MQTT-команд
  =====================================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>

// ==========================================
// НАЛАШТУВАННЯ МЕРЕЖІ ТА MQTT
// ==========================================
const char* ssid = "CSN_307";
const char* password = "LAN307LAN";
const char* mqtt_server = "192.168.10.117";
const int mqtt_port = 1883;

WiFiClient espClient;
PubSubClient client(espClient);

// ==========================================
// РОЗПІНОВКА (ESP32-S2 Mini)
// ==========================================

// Світлодіоди
const int LED1_PIN = 17;   // Світлодіод кімнати 1
const int LED2_PIN = 21;   // Світлодіод кімнати 2
const int LED3_PIN = 4;    // Сигнальний світлодіод охорони/аварії (червоний)
const int LED_ARM_PIN = 2; // Жовтий світлодіод індикації режиму охорони
const int PUMP_PIN = 5;    // Світлодіод (імітація водяної помпи)

// Цифрові сенсори та пристрої
const int DHT_PIN = 6;
#define DHTTYPE DHT11
DHT dht(DHT_PIN, DHTTYPE);

const int PIR_PIN = 18;
const int BUZZER_PIN = 16; 
const int SERVO_PIN = 10;
Servo doorServo;

// Аналогові сенсори
const int MQ135_PIN = 12;
const int SOIL_MOISTURE_PIN = 8;

// Піни для SPI та RFID RC522
const int SS_PIN = 33;     
const int MOSI_PIN = 35;   
const int SCK_PIN = 36;    
const int MISO_PIN = 37;   
const int RST_PIN = 38;    

MFRC522 rfid(SS_PIN, RST_PIN);

// ==========================================
// ГЛОБАЛЬНІ ЗМІННІ ТА СТАНИ
// ==========================================
bool isArmed = false;
bool isNightMode = false;
bool isIntrusionAlarm = false;
bool isFireAlarm = false;

// Пам'ять станів пристроїв
bool currentRoom1State = false;
bool currentRoom2State = false;
bool savedRoom1State = false;
bool savedRoom2State = false;
bool currentPumpState = false;
bool isDoorLocked = false; // Фіксуємо стан дверей, щоб не крутити даремно

unsigned long previousMillisSensors = 0;
const long sensorInterval = 5000;

unsigned long previousMillisLedBlink = 0;
bool led3State = false;

const int GAS_THRESHOLD = 4400;
const int SOIL_DRY_THRESHOLD = 2800;
const String ALLOWED_CARD_UID = "DB0EEF0D";

// ==========================================
// ДОПОМІЖНІ ФУНКЦІЇ ЗВУКУ ТА ІНДИКАЦІЇ
// ==========================================

void blinkSignalLedOnce() {
  digitalWrite(LED3_PIN, HIGH);
  delay(200);
  digitalWrite(LED3_PIN, LOW);
}

void playSuccessMelody() {
  tone(BUZZER_PIN, 1000, 100); delay(120);
  tone(BUZZER_PIN, 1500, 100); delay(120);
  tone(BUZZER_PIN, 2000, 150); delay(150);
}

void playAccessDeniedTone() {
  tone(BUZZER_PIN, 250, 400);
}

void setRoom1(bool state, bool publishMqtt = true) {
  if (currentRoom1State == state) return;
  currentRoom1State = state;
  digitalWrite(LED1_PIN, state ? HIGH : LOW);
  if (publishMqtt) client.publish("home/light/room1", state ? "ON" : "OFF");
}

void setRoom2(bool state, bool publishMqtt = true) {
  if (currentRoom2State == state) return;
  currentRoom2State = state;
  digitalWrite(LED2_PIN, state ? HIGH : LOW);
  if (publishMqtt) client.publish("home/light/room2", state ? "ON" : "OFF");
}

void setPump(bool state, bool publishMqtt = true) {
  if (currentPumpState == state) return;
  currentPumpState = state;
  digitalWrite(PUMP_PIN, state ? HIGH : LOW);
  if (publishMqtt) client.publish("home/garden/pump", state ? "ON" : "OFF");
}

// Плавне та безшумне керування дверима
void lockDoor(bool publishMqtt = true) {
  if (isDoorLocked) return; // Якщо вже зачинено — нічого не робимо
  
  doorServo.attach(SERVO_PIN);
  doorServo.write(0); // Зачиняємо
  delay(400);         // Чекаємо завершення повороту
  doorServo.detach(); // Відключаємо ШІМ, щоб мотор НЕ ТРЯСЯ і не гудів
  
  isDoorLocked = true;
  if (publishMqtt) client.publish("home/security/door", "CLOSED");
  Serial.println("[ДВЕРІ] Зачинено (0°)");
}

void unlockDoor(bool publishMqtt = true) {
  if (!isDoorLocked) return; // Якщо вже відчинено — нічого не робимо
  
  doorServo.attach(SERVO_PIN);
  doorServo.write(90); // Відчиняємо
  delay(400);          // Чекаємо завершення повороту
  doorServo.detach();  // Відключаємо ШІМ, щоб мотор НЕ ТРЯСЯ
  
  isDoorLocked = false;
  if (publishMqtt) client.publish("home/security/door", "OPEN");
  Serial.println("[ДВЕРІ] Відчинено (90°)");
}

void armSystem(bool publishMqtt = true) {
  if (isArmed) return; // Захист від повторного спрацювання
  
  isArmed = true;
  isNightMode = false;
  digitalWrite(LED_ARM_PIN, HIGH);

  savedRoom1State = currentRoom1State;
  savedRoom2State = currentRoom2State;
  setRoom1(false, true);
  setRoom2(false, true);

  lockDoor(true);
  blinkSignalLedOnce();
  Serial.println("Систему ВЗЯТО під охорону.");
  if (publishMqtt) client.publish("home/security/mode", "ARM");
}

void enableNightMode(bool publishMqtt = true) {
  if (isNightMode) return;
  
  isNightMode = true;
  isArmed = false;
  digitalWrite(LED_ARM_PIN, LOW);

  setRoom1(false, true);
  setRoom2(false, true);
  setPump(false, true);

  lockDoor(true);
  blinkSignalLedOnce();
  Serial.println("Увімкнено НІЧНИЙ РЕЖИМ.");
  if (publishMqtt) client.publish("home/security/mode", "NIGHT");
}

void disarmSystem(bool publishMqtt = true) {
  if (!isArmed && !isNightMode && !isIntrusionAlarm && !isFireAlarm) return;

  bool wasArmed = isArmed;
  isArmed = false;
  isNightMode = false;
  isIntrusionAlarm = false;
  isFireAlarm = false; 
  
  noTone(BUZZER_PIN);
  digitalWrite(LED_ARM_PIN, LOW);
  digitalWrite(LED3_PIN, LOW);
  
  unlockDoor(true);

  if (wasArmed) {
    if (savedRoom1State) setRoom1(true, true);
    if (savedRoom2State) setRoom2(true, true);
  }

  blinkSignalLedOnce();
  Serial.println("Систему ЗНЯТО з охорони / нічного режиму.");
  if (publishMqtt) client.publish("home/security/mode", "DISARM");
}

// ==========================================
// МЕРЕЖА ТА MQTT
// ==========================================

void setup_wifi() {
  delay(10);
  Serial.println();
  Serial.print("Підключення до Wi-Fi: ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWi-Fi підключено!");
  Serial.print("IP адреса: ");
  Serial.println(WiFi.localIP());
}

void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }
  Serial.print("MQTT Команда ["); Serial.print(topic); Serial.print("]: "); Serial.println(msg);

  // Передаємо false другим параметром, щоб ПЛАТА НЕ ВІДПРАВЛЯЛА ЕХО-ВІДПОВІДЬ і не зациклювалась!
  if (String(topic) == "home/light/room1") {
    setRoom1(msg == "ON", false);
  }
  else if (String(topic) == "home/light/room2") {
    setRoom2(msg == "ON", false);
  }
  else if (String(topic) == "home/garden/pump") {
    setPump(msg == "ON", false);
  }
  else if (String(topic) == "home/security/door_cmd" || String(topic) == "home/security/door") {
    if (msg == "LOCK" || msg == "CLOSED") lockDoor(false);
    else if (msg == "UNLOCK" || msg == "OPEN") unlockDoor(false);
  }
  else if (String(topic) == "home/security/mode") {
    if (msg == "ARM") armSystem(false);
    else if (msg == "DISARM") disarmSystem(false);
    else if (msg == "NIGHT") enableNightMode(false);
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Спроба підключення до MQTT...");
    if (client.connect("ESP32Client-SmartHome")) {
      Serial.println("Підключено до брокера!");
      client.subscribe("home/light/#");
      client.subscribe("home/garden/#");
      client.subscribe("home/security/#");
    } else {
      Serial.print("Помилка підключення, rc=");
      Serial.print(client.state());
      Serial.println(" -> наступна спроба через 5 секунд...");
      delay(5000);
    }
  }
}

// ==========================================
// ІНІЦІАЛІЗАЦІЯ (setup)
// ==========================================

void setup() {
  Serial.begin(115200);

  pinMode(PIR_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED1_PIN, OUTPUT);
  pinMode(LED2_PIN, OUTPUT);
  pinMode(LED3_PIN, OUTPUT);
  pinMode(LED_ARM_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);

  noTone(BUZZER_PIN);
  digitalWrite(LED1_PIN, LOW);
  digitalWrite(LED2_PIN, LOW);
  digitalWrite(LED3_PIN, LOW);
  digitalWrite(LED_ARM_PIN, LOW);
  digitalWrite(PUMP_PIN, LOW);

  // Початкове встановлення дверей у відчинений стан
  isDoorLocked = true; // Штучно ставимо true, щоб функція unlockDoor спрацювала при старті
  unlockDoor(true);

  dht.begin();

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  rfid.PCD_Init();
  Serial.println("--- RFID RC522 та Сервопривід ініціалізовано ---");

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

// ==========================================
// ОСНОВНИЙ ЦИКЛ (loop)
// ==========================================

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  unsigned long currentMillis = millis();

  // --- 1. КЕРУВАННЯ СИГНАЛЬНИМ СВІТЛОДІОДОМ LED3 ---
  if (isFireAlarm) {
    if (currentMillis - previousMillisLedBlink >= 200) {
      previousMillisLedBlink = currentMillis;
      led3State = !led3State;
      digitalWrite(LED3_PIN, led3State);
    }
  } else if (isIntrusionAlarm) {
    digitalWrite(LED3_PIN, HIGH);
  } else {
    digitalWrite(LED3_PIN, LOW);
  }

  // --- 2. ЛОГІКА ЗЧИТУВАЧА RFID ---
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uidString = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      uidString += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
      uidString += String(rfid.uid.uidByte[i], HEX);
    }
    uidString.toUpperCase();
    Serial.print("Зчитано RFID UID: ");
    Serial.println(uidString);

    client.publish("home/security/rfid", uidString.c_str());

    if (uidString == ALLOWED_CARD_UID) {
      playSuccessMelody();
      if (isArmed || isNightMode) {
        disarmSystem(true);
      } else {
        armSystem(true);
      }
    } else {
      playAccessDeniedTone();
      Serial.println("УВАГА! Невідома картка доступу.");
    }

    rfid.PICC_HaltA();
  }

  // --- 3. ЛОГІКА ДАТЧИКА РУХУ (PIR) ---
  static bool lastPirState = false;
  bool currentPirState = digitalRead(PIR_PIN);
  
  if (currentPirState != lastPirState) {
    lastPirState = currentPirState;
    
    if (currentPirState == HIGH) {
      Serial.println("[PIR] -> Зафіксовано РУХ!");
      client.publish("home/security/motion", "ON");
      
      if (isArmed && !isNightMode) {
        Serial.println("[УВАГА!] Тривога проникнення! Ввімкнено сирену.");
        isIntrusionAlarm = true;
        tone(BUZZER_PIN, 2000);
        client.publish("home/security/alarm", "MOTION_DETECTED");
      }
    } else {
      Serial.println("[PIR] -> Спокій.");
      client.publish("home/security/motion", "OFF");
      
      if (!isArmed && !isFireAlarm) {
        noTone(BUZZER_PIN);
      }
    }
  }

  // --- 4. ПЕРІОДИЧНЕ ОПИТУВАННЯ СЕНСОРІВ (Кожні 5 секунд) ---
  if (currentMillis - previousMillisSensors >= sensorInterval) {
    previousMillisSensors = currentMillis;

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      client.publish("home/climate/temperature", String(t).c_str());
      client.publish("home/climate/humidity", String(h).c_str());
    }

    int gasLevel = analogRead(MQ135_PIN);
    client.publish("home/safety/gas", String(gasLevel).c_str());
    
    if (gasLevel > GAS_THRESHOLD) {
      isFireAlarm = true;
      client.publish("home/safety/alarm", "FIRE_DETECTED");
      tone(BUZZER_PIN, 2500); 

      setRoom1(false, true);
      setRoom2(false, true);
    } else {
      if (isFireAlarm) {
        isFireAlarm = false;
        if (!isIntrusionAlarm) noTone(BUZZER_PIN);
      }
    }

    // --- ОПИТУВАННЯ ҐРУНТУ ТА КЕРУВАННЯ ПОМПОЮ ---
    int soilMoisture = analogRead(SOIL_MOISTURE_PIN);
    client.publish("home/garden/soil", String(soilMoisture).c_str());

    if (soilMoisture > SOIL_DRY_THRESHOLD && !isNightMode) {
      if (!currentPumpState) {
        Serial.print("[САД] Значення: "); Serial.print(soilMoisture);
        Serial.println(" -> Ґрунт СУХИЙ! Авто-увімкнення помпи.");
        setPump(true, true);
      }
    } else if (soilMoisture <= SOIL_DRY_THRESHOLD) {
      if (currentPumpState) {
        Serial.print("[САД] Значення: "); Serial.print(soilMoisture);
        Serial.println(" -> Ґрунт в нормі. Авто-вимкнення помпи.");
        setPump(false, true);
      }
    }
  }
}