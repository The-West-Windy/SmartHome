from flask import Flask, render_template, request, redirect, session, jsonify #додав session та jsonify
import paho.mqtt.client as mqtt
from flask_socketio import SocketIO
import sqlite3
import smtplib
from email.mime.text import MIMEText
from email.mime.multipart import MIMEMultipart
from werkzeug.security import generate_password_hash, check_password_hash
import time
import json
import threading

app = Flask(__name__)

app.secret_key = 'secret_key_123' #добавлений код
socketio = SocketIO(app, async_mode='threading')

def init_db():
    with sqlite3.connect('smarthome.db') as conn:
        c = conn.cursor()
        c.execute('''CREATE TABLE IF NOT EXISTS sensor_data
                     (id INTEGER PRIMARY KEY AUTOINCREMENT,
                      sensor TEXT,
                      value TEXT,
                      timestamp DATETIME DEFAULT (datetime('now', 'localtime')))''')
        c.execute('''CREATE TABLE IF NOT EXISTS activity_log
                     (id INTEGER PRIMARY KEY AUTOINCREMENT,
                      event_type TEXT,
                      message TEXT,
                      timestamp DATETIME DEFAULT (datetime('now', 'localtime')))''')
        c.execute('''CREATE TABLE IF NOT EXISTS users
                     (id INTEGER PRIMARY KEY AUTOINCREMENT,
                      username TEXT UNIQUE NOT NULL,
                      email TEXT UNIQUE NOT NULL,
                      password TEXT NOT NULL,
                      role TEXT DEFAULT 'user')''')
        c.execute('''CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT)''')
        c.execute('''CREATE TABLE IF NOT EXISTS rules (id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, conditions_json TEXT, action_device TEXT, action_state TEXT, action_duration INTEGER, is_active BOOLEAN)''')
        
        # Default settings
        c.execute("INSERT OR IGNORE INTO settings (key, value) VALUES ('email_sender', '')")
        c.execute("INSERT OR IGNORE INTO settings (key, value) VALUES ('email_password', '')")
        c.execute("INSERT OR IGNORE INTO settings (key, value) VALUES ('email_receiver', '')")
        c.execute("INSERT OR IGNORE INTO settings (key, value) VALUES ('email_cooldown', '60')")
        c.execute("INSERT OR IGNORE INTO settings (key, value) VALUES ('gas_threshold', '4200')")
        
        conn.commit()

def save_to_db(sensor, value):
    try:
        with sqlite3.connect('smarthome.db') as conn:
            c = conn.cursor()
            c.execute("INSERT INTO sensor_data (sensor, value) VALUES (?, ?)", (sensor, value))
            conn.commit()
    except Exception as e:
        print(f"DB Error: {e}")

def log_activity(event_type, message):
    try:
        with sqlite3.connect('smarthome.db') as conn:
            c = conn.cursor()
            c.execute("INSERT INTO activity_log (event_type, message) VALUES (?, ?)", (event_type, message))
            conn.commit()
    except Exception as e:
        print(f"DB Error: {e}")

last_email_time = 0

def get_setting(key, default=""):
    try:
        with sqlite3.connect('smarthome.db') as conn:
            c = conn.cursor()
            c.execute("SELECT value FROM settings WHERE key=?", (key,))
            row = c.fetchone()
            return row[0] if row else default
    except:
        return default

def send_email_alert(subject, body):
    global last_email_time
    cooldown = int(get_setting('email_cooldown', '60'))
    if time.time() - last_email_time < cooldown:
        return

    sender = get_setting('email_sender')
    password = get_setting('email_password')
    receiver = get_setting('email_receiver')
    
    if not sender or not password or not receiver:
        print("Email settings not configured.")
        return

    msg = MIMEMultipart()
    msg['From'] = sender
    msg['To'] = receiver
    msg['Subject'] = subject
    msg.attach(MIMEText(body, 'plain'))

    def send_async():
        global last_email_time
        try:
            server = smtplib.SMTP('smtp.gmail.com', 587)
            server.starttls()
            server.login(sender, password)
            server.send_message(msg)
            server.quit()
            print(f"[{time.strftime('%H:%M:%S')}] Email успішно відправлено: {subject}")
            last_email_time = time.time()
        except Exception as e:
            print(f"Помилка відправки Email: {e}")
            
    threading.Thread(target=send_async).start()

def send_registration_email(to_email, username):
    sender = get_setting('email_sender')
    password = get_setting('email_password')
    
    if not sender or not password:
        print("Email settings not configured. Cannot send registration email.")
        return False

    subject = "Реєстрація успішна - Розумний Будинок"
    body = (
        f"Вітаємо, {username}!\n\n"
        f"Ваш акаунт у системі \"Розумний будинок\" успішно створено.\n\n"
        f"Логін: {username}\n"
        f"Email: {to_email}\n\n"
        f"Тепер ви можете увійти в систему, використовуючи ваш логін та пароль."
    )

    msg = MIMEMultipart()
    msg['From'] = sender
    msg['To'] = to_email
    msg['Subject'] = subject
    msg.attach(MIMEText(body, 'plain'))

    def send_async():
        try:
            server = smtplib.SMTP('smtp.gmail.com', 587)
            server.starttls()
            server.login(sender, password)
            server.send_message(msg)
            server.quit()
            print(f"[{time.strftime('%H:%M:%S')}] Registration email successfully sent to {to_email}")
        except Exception as e:
            print(f"Помилка відправки Email (реєстрація): {e}")
            
    threading.Thread(target=send_async).start()
    return True

def revert_action(device, old_state):
    topic_map_rev = {
        "room1": "home/light/room1",
        "room2": "home/light/room2",
        "pump": "home/garden/pump",
        "security": "home/security/mode"
    }
    if device in topic_map_rev:
        mqtt_client.publish(topic_map_rev[device], old_state)
        status_key = f"{device}_status" if device != 'security' else 'security_mode'
        smart_home_data[status_key] = old_state
        socketio.emit('sensor_update', {'sensor': status_key, 'value': old_state})
        log_activity('action', f"Automated revert: {device} set to {old_state}")

def evaluate_rules():
    try:
        with sqlite3.connect('smarthome.db') as conn:
            c = conn.cursor()
            c.execute("SELECT name, conditions_json, action_device, action_state, action_duration FROM rules WHERE is_active=1")
            rules = c.fetchall()
            
        for rule in rules:
            name, conditions_json, act_device, act_state, act_dur = rule
            conditions = json.loads(conditions_json)
            
            result = True
            current_logic = "AND"
            
            for item in conditions:
                if "logic" in item:
                    current_logic = item["logic"]
                else:
                    sensor = item["sensor"]
                    op = item["operator"]
                    val = item["value"]
                    
                    c_val = smart_home_data.get(sensor, "0")
                    cond_res = False
                    
                    try:
                        f_c = float(c_val)
                        f_t = float(val)
                        if op == '>': cond_res = f_c > f_t
                        elif op == '<': cond_res = f_c < f_t
                        elif op == '==': cond_res = f_c == f_t
                        elif op == '!=': cond_res = f_c != f_t
                    except ValueError:
                        c_str = str(c_val).upper()
                        t_str = str(val).upper()
                        if op == '==': cond_res = c_str == t_str
                        elif op == '!=': cond_res = c_str != t_str
                        
                    if current_logic == "AND": result = result and cond_res
                    elif current_logic == "OR": result = result or cond_res
                    
            if result:
                status_key = f"{act_device}_status" if act_device != 'security' else 'security_mode'
                if smart_home_data.get(status_key) != act_state:
                    topic_map_rev = {"room1": "home/light/room1", "room2": "home/light/room2", "pump": "home/garden/pump", "security": "home/security/mode"}
                    if act_device in topic_map_rev:
                        old_state = smart_home_data.get(status_key, "OFF")
                        mqtt_client.publish(topic_map_rev[act_device], act_state)
                        smart_home_data[status_key] = act_state
                        socketio.emit('sensor_update', {'sensor': status_key, 'value': act_state})
                        log_activity('action', f"Rule '{name}' triggered: {act_device} -> {act_state}")
                        
                        if act_dur and int(act_dur) > 0:
                            threading.Timer(int(act_dur) * 60, revert_action, args=[act_device, old_state]).start()
                            
    except Exception as e:
        print(f"Rule Evaluation Error: {e}")

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
    "safety_alarm": "NONE",
    "rfid": "NONE",
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
        "home/security/rfid": "rfid",
        "home/safety/alarm": "safety_alarm",
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
            
        # Логування важливих подій та відправка email
        if sensor_key == 'motion' and payload == 'ON':
            log_activity('alert', 'Motion detected in hall')
            if smart_home_data.get('security_mode') == 'ARM':
                send_email_alert("ТРИВОГА - РУХ", "Зафіксовано несанкціонований рух у будинку (Режим охорони).")
        elif sensor_key == 'door' and payload == 'OPEN':
            log_activity('alert', 'Front door opened')
            if smart_home_data.get('security_mode') == 'ARM':
                send_email_alert("ТРИВОГА - ВТОРГНЕННЯ", "Вхідні двері були відчинені в режимі охорони!")
        elif sensor_key == 'power':
            if payload == 'online':
                log_activity('status', 'Main power restored (220V)')
                send_email_alert("Живлення відновлено", "Основне живлення 220В успішно відновлено.")
            else:
                log_activity('alert', 'Main power lost. Running on battery')
                send_email_alert("БЛЕКАУТ - Розумний Будинок", "Увага! Зникло живлення 220В. Систему переведено на ДБЖ.")
        elif sensor_key == 'alarm':
            if payload == 'MOTION_DETECTED':
                log_activity('alert', 'INTRUSION ALARM TRIGGERED: Motion Detected!')
            elif payload == 'ON':
                log_activity('alert', 'ALARM TRIGGERED!')
        elif sensor_key == 'safety_alarm':
            if payload == 'FIRE_DETECTED':
                log_activity('alert', 'FIRE ALARM TRIGGERED!')
        elif sensor_key == 'rfid':
            log_activity('access', f'RFID Card scanned: {payload}')
        elif sensor_key == 'gas':
            threshold = float(get_setting('gas_threshold', '4200'))
            try:
                if float(payload) > threshold:
                    send_email_alert("КРИТИЧНА ТРИВОГА - ПОЖЕЖА", f"Датчик зафіксував критичний рівень диму/газу ({payload} PPM) в будинку!")
            except ValueError:
                pass
            
        socketio.emit('sensor_update', {'sensor': sensor_key, 'value': payload})
        
        # Evaluate Rules Engine
        evaluate_rules()


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
    session.pop('role', None) #повністю стираємо роль, щоб не можна було повернутися назад
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

    user_role = session.get('role').capitalize()

    if device == "room1":
        mqtt_client.publish("home/light/room1", action.upper())
        smart_home_data["room1_status"] = action.upper()  #фіксуємо новий стан в системі
        log_activity('action', f"Room 1 light turned {action.upper()} by {user_role}")
    elif device == "room2":
        mqtt_client.publish("home/light/room2", action.upper())
        smart_home_data["room2_status"] = action.upper()  #фіксуємо новий стан в системі
        log_activity('action', f"Room 2 light turned {action.upper()} by {user_role}")
    elif device == "pump":
        mqtt_client.publish("home/garden/pump", action.upper())
        smart_home_data["pump_status"] = action.upper()   #фіксуємо новий стан в системі
        log_activity('action', f"Pump turned {action.upper()} by {user_role}")
    elif device == "security":
        #змінювати режим охорони дозволено тільки адміну
        if session.get('role') == 'admin':
            mqtt_client.publish("home/security/mode", action.upper())
            log_activity('action', f"Security mode changed to {action.upper()} by Admin")

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

@app.route('/logs')
def view_logs():
    if session.get('role') not in ['admin', 'user']:
        return redirect('/login')
        
    try:
        with sqlite3.connect('smarthome.db') as conn:
            c = conn.cursor()
            c.execute("SELECT event_type, message, timestamp FROM activity_log ORDER BY timestamp DESC LIMIT 100")
            rows = c.fetchall()
            logs = [{"type": r[0], "message": r[1], "timestamp": r[2]} for r in rows]
    except Exception as e:
        logs = []
        print(f"DB Error fetching logs: {e}")
        
    return render_template('logs.html', logs=logs)


@app.route('/settings', methods=['GET', 'POST'])
def settings_page():
    if session.get('role') != 'admin':
        return redirect('/login')
        
    if request.method == 'POST':
        action = request.form.get('action')
        with sqlite3.connect('smarthome.db') as conn:
            c = conn.cursor()
            if action == 'save_settings':
                for key in ['email_sender', 'email_password', 'email_receiver', 'email_cooldown', 'gas_threshold']:
                    val = request.form.get(key, '')
                    c.execute("UPDATE settings SET value=? WHERE key=?", (val, key))
            elif action == 'add_rule':
                name = request.form.get('name')
                conditions_json = request.form.get('conditions_json')
                act_device = request.form.get('action_device')
                act_state = request.form.get('action_state')
                act_dur = request.form.get('action_duration', 0)
                if not act_dur: act_dur = 0
                c.execute("INSERT INTO rules (name, conditions_json, action_device, action_state, action_duration, is_active) VALUES (?, ?, ?, ?, ?, 1)", 
                          (name, conditions_json, act_device, act_state, act_dur))
            elif action == 'delete_rule':
                rule_id = request.form.get('rule_id')
                c.execute("DELETE FROM rules WHERE id=?", (rule_id,))
            elif action == 'toggle_rule':
                rule_id = request.form.get('rule_id')
                state = request.form.get('state')
                is_active = 1 if state == 'true' else 0
                c.execute("UPDATE rules SET is_active=? WHERE id=?", (is_active, rule_id))
            conn.commit()
        return redirect('/settings')
        
    with sqlite3.connect('smarthome.db') as conn:
        c = conn.cursor()
        c.execute("SELECT key, value FROM settings")
        settings_dict = {row[0]: row[1] for row in c.fetchall()}
        
        c.execute("SELECT id, name, conditions_json, action_device, action_state, action_duration, is_active FROM rules")
        rules = [{"id": r[0], "name": r[1], "conditions_json": r[2], "action_device": r[3], "action_state": r[4], "action_duration": r[5], "is_active": r[6]} for r in c.fetchall()]
        
    return render_template('settings.html', settings=settings_dict, rules=rules)


if __name__ == '__main__':
    socketio.run(app, host='0.0.0.0', port=5000, allow_unsafe_werkzeug=True)