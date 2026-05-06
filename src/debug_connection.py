import socket

ESP32_IP = '10.107.109.95'  # Make sure this matches the IP from your serial monitor
PORT = 1234

try:
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect((ESP32_IP, PORT))
    print("Connected! Waiting for data...\n")
    while True:
        data = sock.recv(1024)
        if not data:
            print("Connection closed by ESP32.")
            break
        # Print the raw bytes, then try to decode and strip
        print(f"RAW: {data}")
        line = data.decode(errors='replace').strip()
        if line:
            print(f"DECODED: {line}")
except Exception as e:
    print(f"ERROR: {e}")