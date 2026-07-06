/*
  =====================================================================
  РОЗУМНИЙ БУДИНОК НА ESP32-S2 mini (MQTT + RFID + SERVO + ALARMS + PUMP)
  Оновлена версія: Жовтий LED охорони, Нічний режим, Пам'ять світла
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
const int LED1_PIN = 17;   // Світлодіод кімнати 1
const int LED2_PIN = 21;   // Світлодіод кімнати 2
const int LED3_PIN = 4;    // Сигнальний світлодіод аварії/тривоги (червоний)
const int LED_ARM_PIN = 2; // Жовтий світлодіод індикації охорони
const int PUMP_PIN = 5;    // Світлодіод / реле водяної помпи

const int DHT_PIN = 6;
#define DHTTYPE DHT11
DHT dht(DHT_PIN, DHTTYPE);

const int PIR_PIN = 18;
const int BUZZER_PIN = 16; 
const int SERVO_PIN = 10;
Servo doorServo;

const int MQ135_PIN = 12;
const int SOIL_MOISTURE_PIN = 8;

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

// Пам'ять станів перед вмиканням охорони
bool savedRoom1State = false;
bool savedRoom2State = false;
bool currentRoom1State = false;
bool currentRoom2State = false;
bool currentPumpState = false;
bool isDoorLocked = false;

unsigned long previousMillisSensors = 0;
const long sensorInterval = 5000;

unsigned long previousMillisLedBlink = 0;
bool led3State = false;

const int GAS_THRESHOLD = 4400;
const int SOIL_DRY_THRESHOLD = 2800;
const String ALLOWED_CARD_UID = "DB0EEF0D";

// ==========================================
// ДОПОМІЖНІ ФУНКЦІЇ КЕРУВАННЯ
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

void setRoom1(bool state) {
  currentRoom1State = state;
  digitalWrite(LED1_PIN, state ? HIGH : LOW);
  client.publish("home/light/room1", state ? "ON" : "OFF");
}

void setRoom2(bool state) {
  currentRoom2State = state;
  digitalWrite(LED2_PIN, state ? HIGH : LOW);
  client.publish("home/light/room2", state ? "ON" : "OFF");
}

void setPump(bool state) {
  currentPumpState = state;
  digitalWrite(PUMP_PIN, state ? HIGH : LOW);
  client.publish("home/garden/pump", state ? "ON" : "OFF");
}

void lockDoor() {
  doorServo.write(0); // Зачиняємо
  isDoorLocked = true;
  client.publish("home/security/door_lock", "LOCKED");
}

void unlockDoor() {
  doorServo.write(90); // Відчиняємо
  isDoorLocked = false;
  client.publish("home/security/door_lock", "UNLOCKED");
}

void armSystem() {
  if (!isArmed) {
    isArmed = true;
    isNightMode = false;
    digitalWrite(LED_ARM_PIN, HIGH); // Увімкнути жовтий LED охорони
    
    // Запам'ятовуємо поточний стан світла
    savedRoom1State = currentRoom1State;
    savedRoom2State = currentRoom2State;
    
    // Вимикаємо світло та синхронізуємо з веб-інтерфейсом
    if (currentRoom1State) setRoom1(false);
    if (currentRoom2State) setRoom2(false);

    lockDoor();
    blinkSignalLedOnce();
    Serial.println("Систему ВЗЯТО під охорону.");
    client.publish("home/security/mode", "ARM");
  }
}

void enableNightMode() {
  isNightMode = true;
  isArmed = false; // Нічний режим працює окремо від повної охорони
  digitalWrite(LED_ARM_PIN, LOW);
  
  // Вимикаємо освітлення і помпу, зачиняємо двері
  if (currentRoom1State) setRoom1(false);
  if (currentRoom2State) setRoom2(false);
  if (currentPumpState) setPump(false);
  
  lockDoor();
  blinkSignalLedOnce();
  Serial.println("Увімкнено НІЧНИЙ РЕЖИМ.");
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
    digitalWrite(LED_ARM_PIN, LOW); // Вимкнути жовтий LED охорони
    digitalWrite(LED3_PIN, LOW);
    
    unlockDoor();
    
    // Відновлення світла, якщо воно горіло до охорони
    if (wasArmed) {
      if (savedRoom1State) setRoom1(true);
      if (savedRoom2State) setRoom2(true);
    }

    blinkSignalLedOnce();
    Serial.println("Систему ЗНЯТО з охорони / нічного режиму.");
    client.publish("home/security/mode", "DISARM");
  }
}

// ==========================================
// МЕРЕЖА ТА MQTT CALLBACK
// ==========================================

void setup_wifi() {
  delay(10);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  Serial.println("\nWi-Fi підключено!");
}

void callback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (int i = 0; i < length; i++) msg += (char)payload[i];

  if (String(topic) == "home/light/room1") {
    setRoom1(msg == "ON");
  }
  else if (String(topic) == "home/light/room2") {
    setRoom2(msg == "ON");
  }
  else if (String(topic) == "home/garden/pump") {
    setPump(msg == "ON");
  }
  else if (String(topic) == "home/security/door_cmd") {
    if (msg == "LOCK") lockDoor();
    else if (msg == "UNLOCK") unlockDoor();
  }
  else if (String(topic) == "home/security/mode") {
    if (msg == "ARM") armSystem();
    else if (msg == "DISARM") disarmSystem();
    else if (msg == "NIGHT") enableNightMode();
  }
}

void reconnect() {
  while (!client.connected()) {
    if (client.connect("ESP32Client-SmartHome")) {
      client.subscribe("home/light/room1");
      client.subscribe("home/light/room2");
      client.subscribe("home/garden/pump");
      client.subscribe("home/security/door_cmd");
      client.subscribe("home/security/mode");
    } else {
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
  unlockDoor();

  dht.begin();
  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  rfid.PCD_Init();

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

// ==========================================
// ОСНОВНИЙ ЦИКЛ (loop)
// ==========================================

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  unsigned long currentMillis = millis();

  // 1. Індикація аварій (LED3)
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

  // 2. Логіка RFID (Перемикання Охорона <-> Знято)
  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uidString = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      uidString += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "") + String(rfid.uid.uidByte[i], HEX);
    }
    uidString.toUpperCase();
    client.publish("home/security/rfid", uidString.c_str());

    if (uidString == ALLOWED_CARD_UID) {
      playSuccessMelody();
      // Якщо охорона чи нічний режим увімкнені — знімаємо, інакше — ставимо на охорону
      if (isArmed || isNightMode) {
        disarmSystem();
      } else {
        armSystem();
      }
    } else {
      playAccessDeniedTone();
    }
    rfid.PICC_HaltA();
  }

  // 3. Логіка Датчика руху (PIR)
  static bool lastPirState = false;
  bool currentPirState = digitalRead(PIR_PIN);
  
  if (currentPirState != lastPirState) {
    lastPirState = currentPirState;
    if (currentPirState == HIGH) {
      client.publish("home/security/motion", "ON");
      // Сирена спрацьовує тільки якщо режим охорони ARM (У Нічному режимі PIR ігнорується!)
      if (isArmed && !isNightMode) {
        isIntrusionAlarm = true;
        tone(BUZZER_PIN, 2000);
        client.publish("home/security/alarm", "MOTION_DETECTED");
      }
    } else {
      client.publish("home/security/motion", "OFF");
      if (!isArmed && !isFireAlarm) noTone(BUZZER_PIN);
    }
  }

  // 4. Опитування сенсорів (Кожні 5 сек)
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
      setRoom1(false);
      setRoom2(false);
    } else if (isFireAlarm) {
      isFireAlarm = false;
      if (!isIntrusionAlarm) noTone(BUZZER_PIN);
    }

    // Опитування ґрунту (Якщо ґрунт сухий і ми не в нічному режимі — вмикаємо помпу)
    int soilMoisture = analogRead(SOIL_MOISTURE_PIN);
    client.publish("home/garden/soil", String(soilMoisture).c_str());

    if (soilMoisture > SOIL_DRY_THRESHOLD && !isNightMode) {
      if (!currentPumpState) setPump(true);
    } else if (soilMoisture <= SOIL_DRY_THRESHOLD && currentPumpState) {
      setPump(false);
    }
  }
}