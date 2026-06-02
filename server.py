#!/usr/bin/python

import socket
import os
import struct
import cv2
import numpy as np
import threading
import sys

HOST = '0.0.0.0'  
PORT = 8888       
SAVE_DIR = "./greenhouse_images"
OTA_FILE = "./firmware.bin"

if not os.path.exists(SAVE_DIR): os.makedirs(SAVE_DIR)

format_requested_flag = False
ota_requested_flag = False

def get_max_saved_index():
    max_index = 0
    for filename in os.listdir(SAVE_DIR):
        if filename.endswith(".jpg"):
            try:
                idx = int(filename.split(".")[0])
                if idx > max_index: max_index = idx
            except: continue
    return max_index

def yuv422_to_jpeg(yuv_bytes, width=1024, height=768):
    raw_array = np.frombuffer(yuv_bytes, dtype=np.uint8)[:width*height*2]
    
    # ФИКС ПАЛИТРЫ ДЛЯ PYTHON-СЕРВЕРА: 
    # Создаем копию массива и программно меняем местами каналы U и V (каждый 2-й и 4-й байт)
    yuv_fixed = bytearray(raw_array)
    for i in range(0, len(yuv_fixed), 4):
        if i + 3 < len(yuv_fixed):
            tmp_v = yuv_fixed[i + 1]
            yuv_fixed[i + 1] = yuv_fixed[i + 3]
            yuv_fixed[i + 3] = tmp_v

    # Превращаем обратно в матрицу numpy
    yuv_matrix = np.frombuffer(yuv_fixed, dtype=np.uint8).reshape((height, width, 2))
    
    # Теперь нативный конвертер OpenCV соберет картинку идеально
    bgr_image = cv2.cvtColor(yuv_matrix, cv2.COLOR_YUV2BGR_YUYV)
    return bgr_image

def handle_client(conn, addr):
    global format_requested_flag, ota_requested_flag
    try:
        conn.settimeout(8.0)
        header = conn.recv(16) # Читаем полноценные 16 байт
        if not header or len(header) < 16: return
            
        img_index, img_size, bat_mv, free_mb = struct.unpack("<IIII", header)
        
        bat_v = bat_mv / 1000.0
        bat_info = f"{bat_v:.2f}V" if bat_mv > 0 else "USB"
        sd_info = f"{free_mb/1024:.2f} GB свободно" if free_mb > 0 else "Ошибка карты"

        # СЦЕНАРИЙ А: Холостой пинг синхронизации
        if img_size == 0:
            if ota_requested_flag:
                if not os.path.exists(OTA_FILE):
                    print(f"[-] Ошибка: {OTA_FILE} не найден!")
                    ota_requested_flag = False
                    return
                ota_size = os.path.getsize(OTA_FILE)
                conn.sendall(f"OTA:{ota_size}".encode())
                print(f"[!] Сигнал OTA ({ota_size} байт) отправлен на плату. Ожидание READY...")
                try:
                    if b"READY" in conn.recv(5):
                        print("[*] Передача бинарника прошивки...")
                        with open(OTA_FILE, "rb") as f: conn.sendall(f.read())
                        print("[+] Прошивка успешно передана!")
                except Exception as e: print(f"[-] Ошибка OTA: {e}")
                ota_requested_flag = False
            elif format_requested_flag:
                conn.sendall(b"FORMAT_SD\n")
                format_requested_flag = False
                print(f"[!!!] Сигнал FORMAT_SD отправлен на плату {addr}.")
            else:
                tx_index = get_max_saved_index()
                conn.sendall(struct.pack("<i", tx_index))
                print(f"[*] Пинг от {addr} ({bat_info}, {sd_info}). Очередь ждет №{tx_index+1:05d}")
            return


        # СЦЕНАРИЙ Б: Прием кадра
        print(f"[+] Прием кадра №{img_index:05d} ({img_size} байт). Свободно на SD: {sd_info}")
        data_buffer = bytearray()
        while len(data_buffer) < img_size:
            packet = conn.recv(4096)
            if not packet: break
            data_buffer.extend(packet)
        
        if len(data_buffer) >= img_size * 0.9:
            filepath = os.path.join(SAVE_DIR, f"{img_index:05d}.jpg")
            bgr = yuv422_to_jpeg(data_buffer, width=1024, height=768)
            cv2.imwrite(filepath, bgr, [int(cv2.IMWRITE_JPEG_QUALITY), 92])
            conn.sendall(struct.pack("<i", img_index))
            print(f"[+] Кадр {img_index:05d}.jpg сохранен успешно.")
    except Exception as e: pass
    finally:
        try: conn.close()
        except: pass

def console_listener():
    global format_requested_flag, ota_requested_flag
    while True:
        cmd = input().strip().lower()
        if cmd == "format":
            format_requested_flag = True
            print("[!!!] Взведен запрос на форматирование карты памяти теплицы!")
        elif cmd == "firmware":
            ota_requested_flag = True
            print(f"[!] Взведен запрос на беспроводное обновление прошивки ({OTA_FILE})")

def start_server():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        server_socket.bind(('0.0.0.0', 8888))
        server_socket.listen(15)
        print(f"[*] TCP Консольный Сервер запущен на порту 8888.")
        threading.Thread(target=console_listener, daemon=True).start()
        while True:
            conn, addr = server_socket.accept()
            threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()
    except KeyboardInterrupt: print("\n[*] Сервер остановлен.")
    finally:
        server_socket.close()
        sys.exit(0)

if __name__ == "__main__": start_server()

