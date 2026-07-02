import paho.mqtt.client as mqtt
import smtplib
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
import time

MQTT_BROKER = "test.mosquitto.org"
MQTT_PORT = 1883

EMAIL_SENDER = "[EMAIL_ADDRESS]"
EMAIL_PASSWORD = "[PASSWORD]" 
EMAIL_RECEIVER = "[EMAIL_ADDRESS]"

last_email_time = 0
EMAIL_COOLDOWN = 60 

def send_email_alert(subject, body):
    global last_email_time
    if time.time() - last_email_time < EMAIL_COOLDOWN:
        return

    msg = MIMEMultipart()
    msg['From'] = EMAIL_SENDER
    msg['To'] = EMAIL_RECEIVER
    msg['Subject'] = subject
    msg.attach(MIMEText(body, 'plain'))

    try:
        server = smtplib.SMTP('smtp.gmail.com', 587)
        server.starttls()
        server.login(EMAIL_SENDER, EMAIL_PASSWORD)
        server.send_message(msg)
        server.quit()
        print(f"[{time.strftime('%H:%M:%S')}] Email успішно відправлено: {subject}")
        last_email_time = time.time()
    except Exception as e:
        print(f"Помилка відправки Email: {e}")

def on_connect(client, userdata, flags, reason_code, properties):
    print(f"Підключено до MQTT-брокера з кодом: {reason_code}")
    client.subscribe("home/#")

def on_message(client, userdata, msg):
    topic = msg.topic
    payload = msg.payload.decode('utf-8')
    print(f"Отримано дані [{topic}]: {payload}")

    if topic == "home/power/status" and payload == "offline":
        print("!!! БЛЕКАУТ !!! Переходимо в режим збереження енергії.")
        send_email_alert("БЛЕКАУТ - Розумний Будинок", "Увага! Зникло живлення 220В. Систему переведено на ДБЖ.")
    
    elif topic == "home/power/status" and payload == "online":
        send_email_alert("Живлення відновлено", "Основне живлення 220В успішно відновлено.")

    elif topic == "home/safety/alarm" and payload == "FIRE_DETECTED":
        print("!!! ПОЖЕЖА !!!")
        send_email_alert("КРИТИЧНА ТРИВОГА - ПОЖЕЖА", "Датчик зафіксував критичний рівень диму/газу в будинку!")

    elif topic == "home/security/alarm":
        if payload == "MOTION_DETECTED":
            send_email_alert("ТРИВОГА - РУХ", "Зафіксовано несанкціонований рух у будинку (Режим охорони).")
        elif payload == "DOOR_BREACH":
            send_email_alert("ТРИВОГА - ВТОРГНЕННЯ", "Вхідні двері були відчинені в режимі охорони!")

print("Запуск сервера Розумного Будинку...")

client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message

try:
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_forever()
except KeyboardInterrupt:
    print("\nЗупинка сервера...")
    client.disconnect()