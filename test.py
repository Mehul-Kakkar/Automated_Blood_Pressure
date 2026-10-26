
import serial
import time

ser = serial.Serial("COM9", 230400, timeout=2)
ser.reset_input_buffer()

print("Listening to ESP32-S3...")
start = time.time()
data = ser.read(100)

print("Bytes received:", len(data))
print("Raw bytes:", data[:40])
print("Elapsed:", round(time.time() - start, 2), "seconds")

ser.close()
