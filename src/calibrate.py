import tkinter as tk
from tkinter import ttk, messagebox
import socket

ESP_IP = '10.107.109.95'   # change to your ESP32 IP
PORT = 1234

class CalibrationApp:
    def __init__(self, root):
        self.root = root
        root.title("Flex Sensor Calibration")
        self.sock = None

        # UI Controls (create first so logging works)
        ttk.Label(root, text="Sensor:").grid(row=0, column=0, padx=5, pady=5)
        self.sensor_var = tk.StringVar(value="Flex0")
        ttk.Combobox(root, textvariable=self.sensor_var, values=["Flex0", "Flex1"]).grid(row=0, column=1, padx=5)

        ttk.Label(root, text="Angle (0-180):").grid(row=1, column=0, padx=5, pady=5)
        self.angle_var = tk.StringVar(value="0")
        ttk.Entry(root, textvariable=self.angle_var).grid(row=1, column=1, padx=5)

        ttk.Button(root, text="Record", command=self.record).grid(row=2, column=0, columnspan=2, pady=5)
        ttk.Button(root, text="Save & Exit", command=self.save_exit).grid(row=3, column=0, columnspan=2, pady=5)

        self.log_text = tk.Text(root, height=10, width=50)
        self.log_text.grid(row=4, column=0, columnspan=2, padx=5, pady=5)

        # Now connect (logging works)
        self.connect(20)

    def connect(self, timeout):
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(timeout)
            self.sock.connect((ESP_IP, PORT))
            self.send_cmd("CAL:START")
            resp = self.recv_line()
            self.log(resp)
        except Exception as e:
            self.log(f"Connection failed: {e}")

    def send_cmd(self, cmd):
        self.sock.send((cmd + "\n").encode())

    def recv_line(self):
        data = b""
        while not data.endswith(b"\n"):
            chunk = self.sock.recv(1)
            if not chunk:
                break
            data += chunk
        return data.decode().strip()

    def record(self):
        angle = self.angle_var.get()
        sensor = self.sensor_var.get()
        cmd = f"CAL:RECORD_{sensor.upper()} {angle}"
        self.send_cmd(cmd)
        resp = self.recv_line()
        self.log(f"Recorded {sensor} at {angle}° → {resp}")

    def save_exit(self):
        self.send_cmd("CAL:DONE")
        resp = self.recv_line()
        self.log(resp)
        self.sock.close()
        self.root.destroy()

    def log(self, msg):
        self.log_text.insert(tk.END, msg + "\n")
        self.log_text.see(tk.END)

root = tk.Tk()
app = CalibrationApp(root)
root.mainloop()