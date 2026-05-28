#!/usr/bin/python

import socket
import os
import struct
import cv2
import numpy as np
import threading

HOST = '0.0.0.0'  
PORT = 8888       
SAVE_DIR = "./greenhouse_images"

if not os.path.exists(SAVE_DIR):
    os.makedirs(SAVE_DIR)

def get_max_saved_index():
    max_index = 0
    for filename in os.listdir(SAVE_DIR):
        if filename.endswith(".jpg"):
            try:
                # Извлекаем чистое число из имени файла 00046.jpg -> 46
                idx = int(filename.split(".")[0])
                if idx > max_index:
                    max_index = idx
            except:
                continue
    return max_index

def yuv422_to_jpeg(yuv_bytes, width=1024, height=768):
    raw_array = np.frombuffer(yuv_bytes, dtype=np.uint8)
    expected_size = width * height * 2
    if len(raw_array) < expected_size:
        raise ValueError(f"Битый пакет: {len(raw_array)} байт.")
    raw_array = raw_array[:expected_size]
    yuv_matrix = raw_array.reshape((height, width, 2))
    try:
        bgr_image = cv2.cvtColor(yuv_matrix, cv2.COLOR_YUV2BGR_YUYV)
    except:
        bgr_image = cv2.cvtColor(yuv_matrix, cv2.COLOR_YUV2BGR_UYVY)
    return bgr_image

def handle_client(conn, addr):
    try:
        conn.settimeout(5.0)
        header = conn.recv(8)
        if not header or len(header) < 8:
            conn.close()
            return
            
        img_index, img_size = struct.unpack("<II", header)
        
        if img_size == 0:
            tx_index = get_max_saved_index()
            conn.sendall(struct.pack("<i", tx_index))
            conn.close()
            return

        data_buffer = bytearray()
        while len(data_buffer) < img_size:
            packet = conn.recv(4096)
            if not packet:
                break
            data_buffer.extend(packet)
        
        if len(data_buffer) == img_size:
            # Имя файла теперь строго пятизначное: 00046.jpg
            filepath = os.path.join(SAVE_DIR, f"{img_index:05d}.jpg")
            
            if len(data_buffer) > 2 and data_buffer[0] == 0xFF and data_buffer[1] == 0xD8:
                with open(filepath, "wb") as f:
                    f.write(data_buffer)
            else:
                bgr = yuv422_to_jpeg(data_buffer, width=1024, height=768)
                cv2.imwrite(filepath, bgr, [int(cv2.IMWRITE_JPEG_QUALITY), 95])
            print(f"[+] Файл {img_index:05d}.jpg успешно сохранен.")
            
            conn.sendall(struct.pack("<i", img_index))
    except Exception as e:
        pass
    finally:
        conn.close()

def start_server():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind((HOST, PORT))
    server_socket.listen(30)
    print(f"[*] Сервер запущен. Текущий ласт-индекс на диске: {get_max_saved_index():05d}")

    try:
        while True:
            conn, addr = server_socket.accept()
            threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()
    except KeyboardInterrupt:
        print("\n[*] Сервер остановлен.")
    finally:
        server_socket.close()

if __name__ == "__main__":
    start_server()
