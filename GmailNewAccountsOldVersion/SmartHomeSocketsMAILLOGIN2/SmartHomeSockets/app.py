from flask import Flask, render_template, request, redirect, session, jsonify #додав session та jsonify
import paho.mqtt.client as mqtt
from flask_socketio import SocketIO
import sqlite3
import smtplib
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from werkzeug.security import generate_password_hash, check_password_hash

app = Flask(__name__)

app.secret_key = 'secret_key_123' #добавлений код
socketio = SocketIO(app, async_mode='threading')

# --- Налаштування для відправки email-повідомлень (реєстрація/сповіщення) ---
EMAIL_SENDER = "homesecuritysystemchnu@gmail.com"
EMAIL_PASSWORD = "bqap rrab ubst ixdj"


def send_email(to_email, subject, body):
    """Універсальна функція відправки email. Повертає True/False залежно від успіху."""
    msg = MIMEMultipart()
    msg['From'] = EMAIL_SENDER
    msg['To'] = to_email
    msg['Subject'] = subject
    msg.attach(MIMEText(body, 'plain'))

    try:
        server = smtplib.SMTP('smtp.gmail.com', 587)
        server.starttls()
        server.login(EMAIL_SENDER, EMAIL_PASSWORD)
        server.send_message(msg)
        server.quit()
        return True
    except Exception as e:
        print(f"Помилка відправки Email: {e}")
        return False


def send_registration_email(to_email, username):
    subject = "Реєстрація успішна - Розумний Будинок"
    body = (
        f"Вітаємо, {username}!\n\n"
        f"Ваш акаунт у системі \"Розумний будинок\" успішно створено.\n\n"
        f"Логін: {username}\n"
        f"Email: {to_email}\n\n"
        f"Тепер ви можете увійти в систему, використовуючи ваш логін та пароль."
    )
    return send_email(to_email, subject, body)


def init_db():
    with sqlite3.connect('smarthome.db') as conn:
        c = conn.cursor()
        c.execute('''CREATE TABLE IF NOT EXISTS sensor_data
                     (id INTEGER PRIMARY KEY AUTOINCREMENT,
                      sensor TEXT,
                      value TEXT,
                      timestamp DATETIME DEFAULT (datetime('now', 'localtime')))''')
        c.execute('''CREATE TABLE IF NOT EXISTS users
                     (id INTEGER PRIMARY KEY AUTOINCREMENT,
                      username TEXT UNIQUE NOT NULL,
                      email TEXT UNIQUE NOT NULL,
                      password TEXT NOT NULL,
                      role TEXT DEFAULT 'user')''')
        conn.commit()

def save_to_db(sensor, value):
    try:
        with sqlite3.connect('smarthome.db') as conn:
            c = conn.cursor()
            c.execute("INSERT INTO sensor_data (sensor, value) VALUES (?, ?)", (sensor, value))
            conn.commit()
    except Exception as e:
        print(f"DB Error: {e}")

smart_home_data = {
    "temperature": "0.0",
    "humidity": "0",
    "gas": "0",
    "soil": "0",
    "battery": "0",
    "power": "online",
    "motion": "OFF",
    "door": "CLOSED",
    "security_mode": "DISARM",
    "alarm": "NONE",
    "room1_status": "OFF",  #збереження стану для першої кімнати
    "room2_status": "OFF",  #збереження стану для другої кімнати
    "pump_status": "OFF"    #збереження стану для помпи
}

MQTT_BROKER = "test.mosquitto.org"
MQTT_PORT = 1883


def on_connect(client, userdata, flags, reason_code, properties):
    client.subscribe("home/#")


def on_message(client, userdata, msg):
    topic = msg.topic
    payload = msg.payload.decode('utf-8')

    topic_map = {
        "home/climate/temperature": "temperature",
        "home/climate/humidity": "humidity",
        "home/safety/gas": "gas",
        "home/garden/soil": "soil",
        "home/power/battery": "battery",
        "home/power/status": "power",
        "home/security/motion": "motion",
        "home/security/door": "door",
        "home/security/mode": "security_mode",
        "home/security/alarm": "alarm",
        "home/light/room1": "room1_status",
        "home/light/room2": "room2_status",
        "home/garden/pump": "pump_status"
    }

    if topic in topic_map:
        sensor_key = topic_map[topic]
        smart_home_data[sensor_key] = payload
        
        # Зберігаємо історію тільки для числових датчиків
        if sensor_key in ['temperature', 'humidity', 'gas', 'soil']:
            save_to_db(sensor_key, payload)
            
        socketio.emit('sensor_update', {'sensor': sensor_key, 'value': payload})


mqtt_client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message
try:
    init_db() # Ініціалізація бази даних
    mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
    mqtt_client.loop_start()
except Exception as e:
    print(f"MQTT connection failed: {e}. Running without MQTT.")


@app.route('/')
def index():
    #якщо користувач недавно заходив, його зразу на головну сторінку кине
    if 'role' in session: 
        return redirect(f"/{session['role']}") 
    return redirect('/login')


@app.route('/login', methods=['GET', 'POST'])
def login():
    if request.method == 'POST':
        username = request.form.get('username', '').strip()
        password = request.form.get('password', '') #зчитуємо пароль з форми

        #перевірка логіну та паролю (демо-акаунти)
        if username == 'admin' and password == 'admin':
            session['role'] = 'admin'
            session.pop('username', None)
            return redirect('/admin')
        elif username == 'guest': #пускає без пароля за логіном guest
            session['role'] = 'guest'
            session.pop('username', None)
            return redirect('/guest')

        # Перевірка серед зареєстрованих користувачів у базі даних
        with sqlite3.connect('smarthome.db') as conn:
            c = conn.cursor()
            c.execute("SELECT username, password, role FROM users WHERE username = ? OR email = ?",
                      (username, username))
            row = c.fetchone()

        if row is None:
            return render_template('login.html', error="Користувача з таким логіном не знайдено!")

        db_username, password_hash, role = row
        if not check_password_hash(password_hash, password):
            return render_template('login.html', error="Невірний пароль!")

        session['role'] = role
        session['username'] = db_username
        return redirect(f'/{role}')

    return render_template('login.html')


@app.route('/logout') #додав роут для виходу з системи
def logout():
    session.clear() #повністю стираємо сесію, щоб не можна було повернутися назад
    return redirect('/login')


@app.route('/register', methods=['GET', 'POST'])
def register():
    if request.method == 'POST':
        username = request.form.get('username', '').strip()
        email = request.form.get('email', '').strip().lower()
        password = request.form.get('password', '')
        confirm_password = request.form.get('confirm_password', '')

        error = None
        if not username or not email or not password or not confirm_password:
            error = "Будь ласка, заповніть усі поля!"
        elif '@' not in email or '.' not in email:
            error = "Введіть коректну електронну адресу!"
        elif password != confirm_password:
            error = "Паролі не співпадають!"
        else:
            with sqlite3.connect('smarthome.db') as conn:
                c = conn.cursor()
                c.execute("SELECT id FROM users WHERE email = ?", (email,))
                if c.fetchone():
                    error = "Ця електронна пошта вже використовується!"
                else:
                    c.execute("SELECT id FROM users WHERE username = ?", (username,))
                    if c.fetchone():
                        error = "Це ім'я користувача вже зайняте!"

        if error:
            return render_template('register.html', error=error)

        password_hash = generate_password_hash(password)
        try:
            with sqlite3.connect('smarthome.db') as conn:
                c = conn.cursor()
                c.execute("INSERT INTO users (username, email, password, role) VALUES (?, ?, ?, ?)",
                          (username, email, password_hash, 'user'))
                conn.commit()
        except sqlite3.IntegrityError:
            return render_template('register.html', error="Ця електронна пошта або ім'я користувача вже використовується!")

        # Надсилаємо email-підтвердження про успішну реєстрацію
        email_sent = send_registration_email(email, username)
        if email_sent:
            success_message = "Реєстрація успішна! Лист-підтвердження надіслано на вашу пошту. Тепер ви можете увійти."
        else:
            success_message = "Реєстрація успішна! Тепер ви можете увійти. (Не вдалося надіслати лист-підтвердження.)"

        return render_template('login.html', messages=[success_message])

    return render_template('register.html')


@app.route('/admin')
def admin_dashboard():
    #захист сторінки, щоб просто написавши в строку http://127.0.0.1:5000/admin чел не зміг зайти
    if session.get('role') != 'admin':
        return redirect('/login')
    return render_template('admin.html', data=smart_home_data)


@app.route('/user')
def user_dashboard():
    if session.get('role') != 'user':
        return redirect('/login')
    return render_template('user.html', data=smart_home_data)


@app.route('/guest')
def guest_dashboard():
    if session.get('role') not in ['admin', 'user', 'guest']:
        return redirect('/login')
    return render_template('guest.html', data=smart_home_data)


@app.route('/control/<device>/<action>')
def control_device(device, action):
    #гості не мають права клацати реле
    if session.get('role') not in ['admin', 'user']:
        return "Дію заборонено", 403

    if device == "room1":
        mqtt_client.publish("home/light/room1", action.upper())
        smart_home_data["room1_status"] = action.upper()  #фіксуємо новий стан в системі
    elif device == "room2":
        mqtt_client.publish("home/light/room2", action.upper())
        smart_home_data["room2_status"] = action.upper()  #фіксуємо новий стан в системі
    elif device == "pump":
        mqtt_client.publish("home/garden/pump", action.upper())
        smart_home_data["pump_status"] = action.upper()   #фіксуємо новий стан в системі
    elif device == "security":
        #змінювати режим охорони дозволено тільки адміну
        if session.get('role') == 'admin':
            mqtt_client.publish("home/security/mode", action.upper())

    #якщо запит прийшов через JS, повертаємо просто "OK" без перезавантаження сторінки
    if request.referrer is None or request.headers.get('X-Requested-With') == 'XMLHttpRequest':
        return "OK", 200
        
    return redirect(request.referrer or '/admin')

@app.route('/api/history/<sensor>')
def get_history(sensor):
    # API для отримання історичних даних для графіків
    if session.get('role') not in ['admin', 'user', 'guest']:
        return jsonify({"error": "Unauthorized"}), 403
        
    try:
        with sqlite3.connect('smarthome.db') as conn:
            c = conn.cursor()
            # Беремо останні 20 записів
            c.execute("SELECT value, timestamp FROM sensor_data WHERE sensor=? ORDER BY timestamp DESC LIMIT 20", (sensor,))
            rows = c.fetchall()
            
        rows.reverse() # Розвертаємо, щоб старі були зліва, нові справа
        # Відправляємо тільки час у форматі HH:MM:SS для зручності на графіку
        data = [{"value": float(r[0]), "time": r[1].split()[1]} for r in rows if r[0].replace('.','',1).isdigit()]
        return jsonify(data)
    except Exception as e:
        return jsonify({"error": str(e)}), 500


if __name__ == '__main__':
    socketio.run(app, host='0.0.0.0', port=5000, allow_unsafe_werkzeug=True)
