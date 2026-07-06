/*
  =====================================================================
  РОЗУМНИЙ БУДИНОК НА ESP32-S2 mini (MQTT + RFID + SERVO + ALARMS + PUMP)
  Відновлено: Повний лог Wi-Fi/MQTT, оригінальні звуки та сенсори
  Додано: Жовтий LED (GPIO2), Пам'ять світла, Нічний режим, Керування помпою та дверима
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

unsigned long previousMillisSensors = 0;
const long sensorInterval = 5000;

unsigned long previousMillisLedBlink = 0;
bool led3State = false;

const int GAS_THRESHOLD = 4400;
const int SOIL_DRY_THRESHOLD = 2800; // Поріг сухості ґрунту для ввімкнення помпи
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
  tone(BUZZER_PIN, 1000, 100);
  delay(120);
  tone(BUZZER_PIN, 1500, 100);
  delay(120);
  tone(BUZZER_PIN, 2000, 150);
  delay(150);
}

void playAccessDeniedTone() {
  tone(BUZZER_PIN, 250, 400); // Низький короткий звук відмови
}

// Керування світлом із відправкою статусу для синхронізації веб-світчів
void setRoom1(bool state, bool publishMqtt = true) {
  currentRoom1State = state;
  digitalWrite(LED1_PIN, state ? HIGH : LOW);
  if (publishMqtt) client.publish("home/light/room1", state ? "ON" : "OFF");
}

void setRoom2(bool state, bool publishMqtt = true) {
  currentRoom2State = state;
  digitalWrite(LED2_PIN, state ? HIGH : LOW);
  if (publishMqtt) client.publish("home/light/room2", state ? "ON" : "OFF");
}

void setPump(bool state, bool publishMqtt = true) {
  currentPumpState = state;
  digitalWrite(PUMP_PIN, state ? HIGH : LOW);
  if (publishMqtt) client.publish("home/garden/pump", state ? "ON" : "OFF");
}

void lockDoor() {
  doorServo.write(0); // Закриваємо двері
  client.publish("home/security/door", "CLOSED");
  Serial.println("[ДВЕРІ] Зачинено (0°)");
}

void unlockDoor() {
  doorServo.write(90); // Відкриваємо двері
  client.publish("home/security/door", "OPEN");
  Serial.println("[ДВЕРІ] Відчинено (90°)");
}

void armSystem() {
  if (!isArmed) {
    isArmed = true;
    isNightMode = false;
    digitalWrite(LED_ARM_PIN, HIGH); // Вмикаємо жовтий світлодіод

    // Запам'ятовуємо стани та вимикаємо світло (світчі скинуться автоматично)
    savedRoom1State = currentRoom1State;
    savedRoom2State = currentRoom2State;
    if (currentRoom1State) setRoom1(false, true);
    if (currentRoom2State) setRoom2(false, true);

    lockDoor();
    blinkSignalLedOnce();
    Serial.println("Систему ВЗЯТО під охорону. Двері зачинено.");
    client.publish("home/security/mode", "ARM");
  }
}

void enableNightMode() {
  isNightMode = true;
  isArmed = false;
  digitalWrite(LED_ARM_PIN, LOW);

  // Вимикаємо світло, помпу та зачиняємо двері
  if (currentRoom1State) setRoom1(false, true);
  if (currentRoom2State) setRoom2(false, true);
  if (currentPumpState) setPump(false, true);

  lockDoor();
  blinkSignalLedOnce();
  Serial.println("Увімкнено НІЧНИЙ РЕЖИМ. Двері зачинено, PIR не вмикає сирену.");
  client.publish("home/security/mode", "NIGHT");
}

void disarmSystem() {
  if (isArmed || isNightMode || isIntrusionAlarm || isFireAlarm) {
    bool wasArmed = isArmed;
    isArmed = false;
    isNightMode = false;
    isIntrusionAlarm = false;
    isFireAlarm = false; 
    
    noTone(BUZZER_PIN);
    digitalWrite(LED_ARM_PIN, LOW); // Вимикаємо жовтий світлодіод
    digitalWrite(LED3_PIN, LOW);
    
    unlockDoor();

    // Відновлюємо попередній стан світла при знятті з охорони
    if (wasArmed) {
      if (savedRoom1State) setRoom1(true, true);
      if (savedRoom2State) setRoom2(true, true);
    }

    blinkSignalLedOnce();
    Serial.println("Систему ЗНЯТО з охорони / нічного режиму. Двері відчинено.");
    client.publish("home/security/mode", "DISARM");
  }
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

  if (String(topic) == "home/light/room1") {
    setRoom1(msg == "ON", false); // false - щоб не зациклювати відправку
  }
  else if (String(topic) == "home/light/room2") {
    setRoom2(msg == "ON", false);
  }
  else if (String(topic) == "home/garden/pump") {
    setPump(msg == "ON", false);
  }
  else if (String(topic) == "home/security/door_cmd" || String(topic) == "home/security/door") {
    if (msg == "LOCK" || msg == "CLOSED") lockDoor();
    else if (msg == "UNLOCK" || msg == "OPEN") unlockDoor();
  }
  else if (String(topic) == "home/security/mode") {
    if (msg == "ARM") armSystem();
    else if (msg == "DISARM") disarmSystem();
    else if (msg == "NIGHT") enableNightMode();
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Спроба підключення до MQTT...");
    String clientId = "ESP32Client-SmartHome";

    if (client.connect(clientId.c_str())) {
      Serial.println("Підключено до брокера!");
      // Підписуємось на всі необхідні топіки керування
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

  doorServo.attach(SERVO_PIN);
  unlockDoor(); // На старті двері відчинені

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
      // Перемикання: якщо на охороні чи в нічному режимі — знімаємо, інакше ставимо на охорону
      if (isArmed || isNightMode) {
        disarmSystem();
      } else {
        armSystem();
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
      Serial.println("[PIR] -> Зафіксовано РУХ! (Відправка ON в MQTT)");
      client.publish("home/security/motion", "ON");
      
      // Сирена вмикається ТІЛЬКИ в режимі ARM (У Нічному режимі PIR ігнорується для тривоги)
      if (isArmed && !isNightMode) {
        Serial.println("[УВАГА!] Тривога проникнення! Ввімкнено сирену.");
        isIntrusionAlarm = true;
        tone(BUZZER_PIN, 2000);
        client.publish("home/security/alarm", "MOTION_DETECTED");
      }
    } else {
      Serial.println("[PIR] -> Рух завершився / спокій. (Відправка OFF в MQTT)");
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

    // Автоматичне увімкнення тільки якщо ґрунт сухий, помпа ще не ввімкнена і це не нічний режим
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