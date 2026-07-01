#define USE_SIM800L 

#include <WiFi.h>
#include <PubSubClient.h>
//#include <SPI.h>
//#include <MFRC522.h>
#include <DHT.h>
//#include <ESP32Servo.h>

#ifdef USE_SIM800L
  #define TINY_GSM_MODEM_SIM800
  #include <TinyGsmClient.h>
#endif

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
// ОНОВЛЕНА РОЗПІНОВКА (ESP32-S2 Mini)
// ==========================================

// 1. Пряме керування навантаженням (Замість PCF8574)
const int ROOM1_PIN = 15;   // Світлодіод кімнати 1
const int ROOM2_PIN = 16;   // Світлодіод кімнати 2
const int ROOM3_PIN = 17;   // Світлодіод кімнати 3 (або загальне світло)
const int PUMP_PIN  = 21;   // Тепер світлодіод

// 2. SPI (RFID RC522)
//const int SCK_PIN = 18;
//const int MISO_PIN = 12;
//const int MOSI_PIN = 11;
//const int SS_PIN = 5;
//const int RST_PIN = 13;
//MFRC522 rfid(SS_PIN, RST_PIN);

// 3. Цифрові сенсори та модулі
const int DHT_PIN = 6;
#define DHTTYPE DHT11
DHT dht(DHT_PIN, DHTTYPE);

const int PIR_PIN = 40;
//const int REED_PIN = 35;
//const int BLACKOUT_PIN = 34; // Цифровий вхід для детекції 220В

const int BUZZER_PIN = 33;   // Пасивний базер
//const int SERVO_PIN = 36;    // Сервопривід
//Servo doorServo;

//const int RGB_R_PIN = 1;
//const int RGB_G_PIN = 2;
//const int RGB_B_PIN = 3;

// 4. Аналогові сенсори (тільки GPIO 1-20 для S2)
const int MQ135_PIN = 7;
//const int BATTERY_PIN = 4;        
const int SOIL_MOISTURE_PIN = 10; 
const int LDR_PIN = 14;           // Фоторезистор

// 5. UART (SIM800L)
#ifdef USE_SIM800L
  const int SIM800L_RX = 37;
  const int SIM800L_TX = 39;
  const String EMERGENCY_PHONE = "+380000000000";
  TinyGsm modem(Serial1); 
#endif

// ==========================================
// ГЛОБАЛЬНІ ЗМІННІ
// ==========================================
//bool isBlackout = false;
bool isArmed = false;
bool smsSent = false;

unsigned long previousMillisSensors = 0;
const long sensorInterval = 5000;

unsigned long welcomeLightTimer = 0;
bool welcomeLightActive = false;
const unsigned long welcomeDuration = 120000; 

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

  // Оновлена логіка прямого керування світлодіодами
  if (String(topic) == "home/light/room1") {
    if (msg == "ON") {
        digitalWrite(ROOM1_PIN, HIGH); // Вмикаємо світло
        welcomeLightActive = false; 
    }
    else if (msg == "OFF") {
        digitalWrite(ROOM1_PIN, LOW);  // Вимикаємо світло
        welcomeLightActive = false;
    }
  }
  else if (String(topic) == "home/light/room2") {
    if (msg == "ON") digitalWrite(ROOM2_PIN, HIGH);
    else if (msg == "OFF") digitalWrite(ROOM2_PIN, LOW);
  }
  else if (String(topic) == "home/garden/pump") {
    // Пін 21 тепер світлодіод (Active HIGH)
    if (msg == "ON") digitalWrite(PUMP_PIN, HIGH);
    else if (msg == "OFF") digitalWrite(PUMP_PIN, LOW);
  }
  else if (String(topic) == "home/security/mode") {
    if (msg == "ARM") {
      isArmed = true;
      //digitalWrite(RGB_B_PIN, HIGH);
      Serial.println("Систему ВЗЯТО під охорону!");
    }
    else if (msg == "DISARM") {
      isArmed = false;
      //digitalWrite(RGB_B_PIN, LOW);
      noTone(BUZZER_PIN); // Вимикаємо сирену
      Serial.println("Систему ЗНЯТО з охорони.");
    }
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Спроба підключення до MQTT...");
    String clientId = "ESP32Client-SmartHome";
    
    if (client.connect(clientId.c_str())) {
      Serial.println("Підключено до брокера!");
      
      client.subscribe("home/light/room1");
      client.subscribe("home/light/room2");
      client.subscribe("home/garden/pump");
      client.subscribe("home/security/mode");
      
    } else {
      Serial.print("Помилка підключення, rc=");
      Serial.print(client.state());
      delay(5000); 
    }
  }
}

void setup() {
  Serial.begin(115200);

  #ifdef USE_SIM800L
    Serial1.begin(9600, SERIAL_8N1, SIM800L_RX, SIM800L_TX);
    delay(3000);
    modem.restart();
  #endif

  //pinMode(BLACKOUT_PIN, INPUT);            
  pinMode(PIR_PIN, INPUT);                
  //pinMode(REED_PIN, INPUT_PULLUP);        
  
  pinMode(BUZZER_PIN, OUTPUT);            
  //pinMode(RGB_R_PIN, OUTPUT);             
  //pinMode(RGB_G_PIN, OUTPUT);             
  //pinMode(RGB_B_PIN, OUTPUT);             

  // Налаштовуємо нові піни прямого керування
  pinMode(ROOM1_PIN, OUTPUT);
  pinMode(ROOM2_PIN, OUTPUT);
  pinMode(ROOM3_PIN, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);

  noTone(BUZZER_PIN); // Вимикаємо пасивний базер
  //digitalWrite(RGB_R_PIN, LOW);
  //digitalWrite(RGB_G_PIN, LOW);
  //digitalWrite(RGB_B_PIN, LOW);

  // Вимикаємо всі світлодіоди (LOW)
  digitalWrite(ROOM1_PIN, LOW);
  digitalWrite(ROOM2_PIN, LOW);
  digitalWrite(ROOM3_PIN, LOW);
  digitalWrite(PUMP_PIN, LOW); // Тепер як світлодіод

  //SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
  //rfid.PCD_Init();

  dht.begin();

  //doorServo.attach(SERVO_PIN);
  //doorServo.write(0);

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); 

  // --- ЛОГІКА БЛЕКАУТУ ---
  /*
  int powerStatus = digitalRead(BLACKOUT_PIN);
  if (powerStatus == LOW && !isBlackout) {
    isBlackout = true;
    client.publish("home/power/status", "offline");
    
    // Аварійно вимикаємо всі споживачі
    digitalWrite(PUMP_PIN, LOW); // Як світлодіод
    digitalWrite(ROOM1_PIN, LOW);
    digitalWrite(ROOM2_PIN, LOW);
    digitalWrite(ROOM3_PIN, LOW);
  } else if (powerStatus == HIGH && isBlackout) {
    isBlackout = false;
    client.publish("home/power/status", "online");
  }
  */

  // --- ЛОГІКА ДАТЧИКА РУХУ ---
  static bool lastPirState = false;
  bool currentPirState = digitalRead(PIR_PIN);
  if (currentPirState != lastPirState) {
    lastPirState = currentPirState;
    if (currentPirState == HIGH) {
      client.publish("home/security/motion", "ON");
      if (isArmed) {
         tone(BUZZER_PIN, 2000); // Вмикаємо звук тривоги (2000 Гц)
         client.publish("home/security/alarm", "MOTION_DETECTED");
      }
    } else {
      client.publish("home/security/motion", "OFF");
      if (!isArmed) noTone(BUZZER_PIN); 
    }
  }

  /*
  // --- ЛОГІКА ГЕРКОНА (ВХІДНІ ДВЕРІ) ---
  static bool lastDoorState = false;
  bool currentDoorState = digitalRead(REED_PIN); 
  if (currentDoorState != lastDoorState) {
    lastDoorState = currentDoorState;
    if (currentDoorState == HIGH) { 
      client.publish("home/security/door", "OPEN");
      
      if (isArmed) {
         tone(BUZZER_PIN, 2000); // Звук тривоги
         client.publish("home/security/alarm", "DOOR_BREACH");
      } else {
         int lightLevel = analogRead(LDR_PIN);
         if (lightLevel > 2000) { 
            digitalWrite(ROOM1_PIN, HIGH); // Вмикаємо світло "Привітання"
            welcomeLightActive = true;
            welcomeLightTimer = millis();
         }
      }
    } else {
      client.publish("home/security/door", "CLOSED");
    }
  }

  if (welcomeLightActive && (millis() - welcomeLightTimer >= welcomeDuration)) {
    digitalWrite(ROOM1_PIN, LOW); // Вимикаємо світло "Привітання"
    welcomeLightActive = false;
  }

  // --- ЛОГІКА RFID ---
  static unsigned long doorOpenedTime = 0;
  static bool doorIsOpen = false;

  if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
    String uidString = "";
    for (byte i = 0; i < rfid.uid.size; i++) {
      uidString += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
      uidString += String(rfid.uid.uidByte[i], HEX);
    }
    uidString.toUpperCase();

    if (uidString == "4376A21A") { 
      client.publish("home/security/access", "GRANTED");
      doorServo.write(90);            
      digitalWrite(RGB_G_PIN, HIGH); 
      
      // Звук успішного доступу (короткий пік 1200 Гц)
      tone(BUZZER_PIN, 1200); delay(100); noTone(BUZZER_PIN); 
      
      doorIsOpen = true;
      doorOpenedTime = millis();     
    } else {
      client.publish("home/security/access", "DENIED");
      digitalWrite(RGB_R_PIN, HIGH); 
      
      // Звук відмови (довгий басовитий гудок 500 Гц)
      tone(BUZZER_PIN, 500); delay(800); noTone(BUZZER_PIN); 
      digitalWrite(RGB_R_PIN, LOW);
    }
    rfid.PICC_HaltA();
  }

  if (doorIsOpen && (millis() - doorOpenedTime >= 5000)) {
    doorServo.write(0);            
    digitalWrite(RGB_G_PIN, LOW);  
    doorIsOpen = false;
  }

    */
  // --- ЛОГІКА ПЕРІОДИЧНИХ СЕНСОРІВ ---
  if (millis() - previousMillisSensors >= sensorInterval) {
    previousMillisSensors = millis();

    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      client.publish("home/climate/temperature", String(t).c_str());
      client.publish("home/climate/humidity", String(h).c_str());
    }

    int gasLevel = analogRead(MQ135_PIN);
    client.publish("home/safety/gas", String(gasLevel).c_str());
    if (gasLevel > 2200) { 
      client.publish("home/safety/alarm", "FIRE_DETECTED");
      tone(BUZZER_PIN, 2000); // Звук пожежної тривоги
      
      // Вимикаємо світло в кімнатах під час пожежі
      digitalWrite(ROOM1_PIN, LOW); 
      digitalWrite(ROOM2_PIN, LOW);
      digitalWrite(ROOM3_PIN, LOW);
      
      if (!smsSent) {
        #ifdef USE_SIM800L
          modem.sendSMS(EMERGENCY_PHONE, "FIRE ALARM! Critical smoke level detected in Smart Home!");
        #endif
        smsSent = true;
      }
    } else {
      // Скидаємо прапорець, якщо газ вивітрився
      smsSent = false;
    }

    int soilMoisture = analogRead(SOIL_MOISTURE_PIN);
    client.publish("home/garden/soil", String(soilMoisture).c_str());

    /*
    int rawBat = analogRead(BATTERY_PIN);
    float voltage = (rawBat / 4095.0) * 3.3 * 2.0; 
    int batteryPct = map(voltage * 100, 320, 420, 0, 100); 
    batteryPct = constrain(batteryPct, 0, 100);
    client.publish("home/power/battery", String(batteryPct).c_str());
    */
  }
}