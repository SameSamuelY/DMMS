import tkinter as tk
from tkinter import ttk, messagebox
import socket
import re

ESP_IP = '10.107.109.95'   # change to your ESP32's IP
PORT = 1234

class CalibrationApp:
    def __init__(self, root):
        self.root = root
        root.title("Flex Sensor Calibration (Editable)")
        self.sock = None

        # Local points store – separate for each sensor
        # Format: list of (raw, angle)
        self.points_flex0 = []
        self.points_flex1 = []

        # Current sensor being edited
        self.current_sensor = tk.StringVar(value="Flex0")

        # --- UI Setup ---
        # Sensor selector
        ttk.Label(root, text="Sensor:").grid(row=0, column=0, padx=5, pady=5, sticky='w')
        self.sensor_combo = ttk.Combobox(root, textvariable=self.current_sensor,
                                         values=["Flex0", "Flex1"], state="readonly")
        self.sensor_combo.grid(row=0, column=1, padx=5, pady=5, sticky='ew')
        self.sensor_combo.bind('<<ComboboxSelected>>', lambda e: self.refresh_table())

        # Angle entry
        ttk.Label(root, text="Angle (0-180):").grid(row=1, column=0, padx=5, pady=5, sticky='w')
        self.angle_var = tk.StringVar(value="0")
        self.angle_entry = ttk.Entry(root, textvariable=self.angle_var, width=8)
        self.angle_entry.grid(row=1, column=1, padx=5, pady=5, sticky='w')

        # Quick angle buttons
        btn_frame = ttk.Frame(root)
        btn_frame.grid(row=2, column=0, columnspan=2, pady=5)
        for ang in [0, 45, 90, 135, 180]:
            ttk.Button(btn_frame, text=str(ang), width=4,
                       command=lambda a=ang: self.set_angle(a)).pack(side='left', padx=2)

        # Record button
        ttk.Button(root, text="Record Current Angle", command=self.record).grid(
            row=3, column=0, columnspan=2, pady=5)

        # Points table
        self.tree = ttk.Treeview(root, columns=('raw', 'angle'), show='headings', height=8)
        self.tree.heading('raw', text='Raw ADC')
        self.tree.heading('angle', text='Angle (°)')
        self.tree.column('raw', width=100, anchor='center')
        self.tree.column('angle', width=100, anchor='center')
        self.tree.grid(row=4, column=0, columnspan=2, padx=5, pady=5, sticky='nsew')

        # Edit / Delete buttons
        edit_frame = ttk.Frame(root)
        edit_frame.grid(row=5, column=0, columnspan=2, pady=5)
        ttk.Button(edit_frame, text="Delete Selected", command=self.delete_selected).pack(side='left', padx=2)
        ttk.Button(edit_frame, text="Clear All", command=self.clear_all).pack(side='left', padx=2)

        # Save & Exit
        ttk.Button(root, text="Save & Exit", command=self.save_exit).grid(
            row=6, column=0, columnspan=2, pady=10)

        # Log area
        self.log_text = tk.Text(root, height=6, width=50)
        self.log_text.grid(row=7, column=0, columnspan=2, padx=5, pady=5, sticky='nsew')

        # Make the table row expandable
        root.grid_rowconfigure(4, weight=1)
        root.grid_columnconfigure(1, weight=1)

        # Connect and fetch current calibration
        self.connect(20)

    # --- Networking helpers ---
    def connect(self, timeout):
        try:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.sock.settimeout(timeout)
            self.sock.connect((ESP_IP, PORT))
            self.log("Connected. Pausing streaming...")
                  
            # Stop sensor data first!
            self.send_cmd("CAL:START")
            resp = self.recv_line()
            self.log(f"Enter calibration: {resp}")
            if resp != "CAL:READY":
                self.log("Warning: Could not enter calibration mode!")
        
            # Now fetch existing calibration table safely
            self.send_cmd("CAL:DUMP")
            resp = self.recv_line()
            self.log(f"ESP: {resp}")
            self.parse_dump(resp)
            self.refresh_table()
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

    def parse_dump(self, dump_str):
        """Parse 'Flex0: raw->angle; ... | Flex1: ...' into local lists."""
        self.points_flex0 = []
        self.points_flex1 = []
        # Split into sensor parts
        parts = dump_str.split('|')
        for part in parts:
            sensor = None
            if 'Flex0:' in part:
                sensor = 0
            elif 'Flex1:' in part:
                sensor = 1
            else:
                continue
            # Extract the points string
            points_str = part.split(':', 1)[1].strip()
            if points_str:
                points = re.findall(r'(\d+)->([\d.]+)', points_str)
                for raw, angle in points:
                    pt = (int(raw), float(angle))
                    if sensor == 0:
                        self.points_flex0.append(pt)
                    else:
                        self.points_flex1.append(pt)
        # Sort by raw ADC ascending
        self.points_flex0.sort(key=lambda x: x[0])
        self.points_flex1.sort(key=lambda x: x[0])

    # --- UI logic ---
    def set_angle(self, angle):
        self.angle_var.set(str(angle))
        # Optionally auto-record immediately? We'll keep it manual.

    def record(self):
        """Record the current angle for the selected sensor (local only)."""
        try:
            angle = float(self.angle_var.get())
            if angle < 0 or angle > 180:
                self.log("Angle must be 0-180")
                return
        except ValueError:
            self.log("Invalid angle")
            return

        # Send command to ESP32 to get raw ADC at this moment
        sensor = self.current_sensor.get()
        cmd = f"CAL:RECORD_{sensor.upper()} {angle}"
        self.send_cmd(cmd)
        resp = self.recv_line()
        self.log(resp)
        # Expected response: "OK:<raw>@<angle>"
        if resp.startswith("OK:"):
            parts = resp[3:].split('@')
            if len(parts) == 2:
                raw = int(parts[0])
                # Add to local list
                pt = (raw, angle)
                if sensor == "Flex0":
                    self.points_flex0.append(pt)
                    self.points_flex0.sort(key=lambda x: x[0])
                else:
                    self.points_flex1.append(pt)
                    self.points_flex1.sort(key=lambda x: x[0])
                self.refresh_table()
        else:
            self.log("Unexpected response: " + resp)

    def delete_selected(self):
        """Delete the selected point(s) from the current sensor's list."""
        selected = self.tree.selection()
        if not selected:
            return
        sensor = self.current_sensor.get()
        points = self.points_flex0 if sensor == "Flex0" else self.points_flex1
        # Collect indices to delete (in reverse order to avoid index shift)
        indices = [int(self.tree.item(item, 'text')) for item in selected]
        indices.sort(reverse=True)
        for idx in indices:
            if 0 <= idx < len(points):
                del points[idx]
        self.refresh_table()

    def clear_all(self):
        """Remove all points for the current sensor."""
        if messagebox.askyesno("Clear All", "Remove all calibration points for this sensor?"):
            sensor = self.current_sensor.get()
            if sensor == "Flex0":
                self.points_flex0.clear()
            else:
                self.points_flex1.clear()
            self.refresh_table()

    def refresh_table(self):
        """Reload the table with points of the current sensor."""
        sensor = self.current_sensor.get()
        points = self.points_flex0 if sensor == "Flex0" else self.points_flex1
        # Clear table
        for item in self.tree.get_children():
            self.tree.delete(item)
        # Populate
        for i, (raw, angle) in enumerate(points):
            self.tree.insert('', 'end', text=str(i), values=(raw, f"{angle:.1f}"))

    def save_exit(self):
        """Send all local points to ESP32 and store in NVS."""
        self.log("Saving calibration to ESP32...")

        for raw, angle in self.points_flex0:
            cmd = f"CAL:RECORD_FLEX0 {angle}"
            self.send_cmd(cmd)
            resp = self.recv_line()
            if not resp.startswith("OK:"):
                self.log(f"Unexpected response for Flex0: {resp}")

        for raw, angle in self.points_flex1:
            cmd = f"CAL:RECORD_FLEX1 {angle}"
            self.send_cmd(cmd)
            resp = self.recv_line()
            if not resp.startswith("OK:"):
                self.log(f"Unexpected response for Flex1: {resp}")

        self.send_cmd("CAL:DONE")
        resp = self.recv_line()
        self.log(f"Save result: {resp}")
        self.sock.close()
        self.root.destroy()

    def log(self, msg):
        self.log_text.insert(tk.END, msg + "\n")
        self.log_text.see(tk.END)

# Run
root = tk.Tk()
app = CalibrationApp(root)
root.mainloop()