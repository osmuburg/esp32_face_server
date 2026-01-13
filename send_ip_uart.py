import serial
import subprocess
import time

# UART de la Raspberry Pi
SERIAL_PORT = "/dev/serial0"
BAUDRATE = 9600

def get_ip():
    try:
        result = subprocess.check_output(
            "hostname -I | awk '{print $1}'",
            shell=True
        )
        return result.decode().strip()
    except:
        return None

def main():
    ser = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=1)
    time.sleep(2)

    print("📡 Enviando IP por UART...")

    last_ip = ""

    while True:
        ip = get_ip()
        if ip and ip != last_ip:
            msg = f"IP:{ip}\n"
            ser.write(msg.encode())
            print("➡️ Enviado:", msg.strip())
            last_ip = ip

        time.sleep(5)

if __name__ == "__main__":
    main()

