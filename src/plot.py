import socket
import re
import time
import threading
import matplotlib.pyplot as plt
from collections import deque

ESP32_IP = '10.107.109.95'
PORT = 1234

max_len = 200
times = deque(maxlen=max_len)
force_vals = deque(maxlen=max_len)
flex0_vals = deque(maxlen=max_len)
flex1_vals = deque(maxlen=max_len)

# Create a lock to protect the deques
data_lock = threading.Lock()

def read_data():
    global times, force_vals, flex0_vals, flex1_vals
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)
    try:
        sock.connect((ESP32_IP, PORT))
        print("Connected to ESP32, receiving data...")
    except Exception as e:
        print(f"Connection error: {e}")
        return

    buffer = ""
    while True:
        try:
            data = sock.recv(1024)
            if not data:
                print("Connection lost.")
                break
            buffer += data.decode(errors='replace')
            while '\r\n' in buffer:
                line, buffer = buffer.split('\r\n', 1)
                line = line.strip()
                if not line:
                    continue
                # Debug: print every line received
                print(f"Line: '{line}'")
                match = re.search(r"FSR:([\d.]+) N, Flex0:\d+ => ([\d.]+) deg, Flex1:\d+ => ([\d.]+) deg", line)
                if match:
                    force = float(match.group(1))
                    f0 = float(match.group(2))
                    f1 = float(match.group(3))
                    print(f"  --> Force={force}, Flex0={f0}, Flex1={f1}")
                    t = time.time()
                    with data_lock:   # lock while appending
                        times.append(t)
                        force_vals.append(force)
                        flex0_vals.append(f0)
                        flex1_vals.append(f1)
                else:
                    print("  (no regex match)")
        except Exception as e:
            print(f"Recv error: {e}")
            break

thread = threading.Thread(target=read_data, daemon=True)
thread.start()

# Give the connection a moment
time.sleep(1)

plt.ion()
fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 8))

while True:
    with data_lock:  # atomic snapshot
        if len(times) > 0:
            t_plot = list(times)
            f_plot = list(force_vals)
            f0_plot = list(flex0_vals)
            f1_plot = list(flex1_vals)
        else:
            t_plot = f_plot = f0_plot = f1_plot = []

    if t_plot:
        ax1.cla()
        ax1.plot(t_plot, f_plot, 'r-', label='Force (N)')
        ax1.set_ylabel('Force (N)')
        ax1.legend(loc='upper left')
        ax1.grid(True)

        ax2.cla()
        ax2.plot(t_plot, f0_plot, 'b-', label='0:Index   (°)')
        ax2.plot(t_plot, f1_plot, 'g-', label='1:Middle (°)')
        ax2.set_xlabel('Time (s)')
        ax2.set_ylabel('Angle (°)')
        ax2.legend(loc='upper left')
        ax2.grid(True)

        plt.tight_layout()
        plt.pause(0.05)
    else:
        plt.pause(0.1)