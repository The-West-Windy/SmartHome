from flask import Flask, render_template, request, redirect, session, jsonify #додав session та jsonify
import paho.mqtt.client as mqtt
from flask_socketio import SocketIO
import sqlite3

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
            
        # Логування важливих подій
        if sensor_key == 'motion' and payload == 'ON':
            log_activity('alert', 'Motion detected in hall')
        elif sensor_key == 'door' and payload == 'OPEN':
            log_activity('alert', 'Front door opened')
        elif sensor_key == 'power':
            if payload == 'online':
                log_activity('status', 'Main power restored (220V)')
            else:
                log_activity('alert', 'Main power lost. Running on battery')
        elif sensor_key == 'alarm' and payload == 'ON':
            log_activity('alert', 'ALARM TRIGGERED!')
            
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
        username = request.form.get('username')
        password = request.form.get('password') #зчитуємо пароль з форми

        #перевірка логіну та паролю
        if username == 'admin' and password == 'admin':
            session['role'] = 'admin'
            return redirect('/admin')
        elif username == 'user' and password == 'user':
            session['role'] = 'user'
            return redirect('/user')
        elif username == 'guest': #пускає без пароля за логіном guest
            session['role'] = 'guest'
            return redirect('/guest')
            
        return render_template('login.html', error="Невірний логін або пароль!")
    return render_template('login.html')


@app.route('/logout') #додав роут для виходу з системи
def logout():
    session.pop('role', None) #повністю стираємо роль, щоб не можна було повернутися назад
    return redirect('/login')


@app.route('/register', methods=['GET', 'POST'])
def register():
    if request.method == 'POST':
        return redirect('/login')
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


if __name__ == '__main__':
    socketio.run(app, host='0.0.0.0', port=5000, allow_unsafe_werkzeug=True)