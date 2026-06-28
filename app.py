from flask import Flask, render_template, request, redirect
import paho.mqtt.client as mqtt

app = Flask(__name__)

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
    "alarm": "NONE"
}

MQTT_BROKER = "127.0.0.1"
MQTT_PORT = 1883

z
def on_connect(client, userdata, flags, rc):
    client.subscribe("home/#")


def on_message(client, userdata, msg):
    topic = msg.topic
    payload = msg.payload.decode('utf-8')

    if topic == "home/climate/temperature":
        smart_home_data["temperature"] = payload
    elif topic == "home/climate/humidity":
        smart_home_data["humidity"] = payload
    elif topic == "home/safety/gas":
        smart_home_data["gas"] = payload
    elif topic == "home/garden/soil":
        smart_home_data["soil"] = payload
    elif topic == "home/power/battery":
        smart_home_data["battery"] = payload
    elif topic == "home/power/status":
        smart_home_data["power"] = payload
    elif topic == "home/security/motion":
        smart_home_data["motion"] = payload
    elif topic == "home/security/door":
        smart_home_data["door"] = payload
    elif topic == "home/security/mode":
        smart_home_data["security_mode"] = payload
    elif topic == "home/security/alarm":
        smart_home_data["alarm"] = payload


mqtt_client = mqtt.Client()
mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message
mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
mqtt_client.loop_start()


@app.route('/')
def index():
    return redirect('/login')


@app.route('/login', methods=['GET', 'POST'])
def login():
    if request.method == 'POST':
        username = request.form.get('username')
        if username == 'admin':
            return redirect('/admin')
        elif username == 'user':
            return redirect('/user')
        return redirect('/guest')
    return render_template('login.html')


@app.route('/register', methods=['GET', 'POST'])
def register():
    if request.method == 'POST':
        return redirect('/login')
    return render_template('register.html')


@app.route('/admin')
def admin_dashboard():
    return render_template('admin.html', data=smart_home_data)


@app.route('/user')
def user_dashboard():
    return render_template('user.html', data=smart_home_data)


@app.route('/guest')
def guest_dashboard():
    return render_template('guest.html', data=smart_home_data)


@app.route('/control/<device>/<action>')
def control_device(device, action):
    if device == "room1":
        mqtt_client.publish("home/light/room1", action.upper())
    elif device == "room2":
        mqtt_client.publish("home/light/room2", action.upper())
    elif device == "pump":
        mqtt_client.publish("home/garden/pump", action.upper())
    elif device == "security":
        mqtt_client.publish("home/security/mode", action.upper())
    return redirect(request.referrer or '/admin')


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)