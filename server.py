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

if not os.path.exists(SAVE_DIR):
    os.makedirs(SAVE_DIR)

format_requested_flag = False
ota_requested_flag = False

def get_max_saved_index():
    max_index = 0
    for filename in os.listdir(SAVE_DIR):
        if filename.endswith(".jpg"):
            try:
                pure_name = filename.split(".")[0]
                idx = int(pure_name)
                if idx > max_index: max_index = idx
            except: continue
    return max_index

def yuv422_to_jpeg(yuv_bytes, width=1024, height=768):
    raw_array = np.frombuffer(yuv_bytes, dtype=np.uint8)[:width*height*2]
    yuv_matrix = raw_array.reshape((height, width, 2))
    try: bgr_image = cv2.cvtColor(yuv_matrix, cv2.COLOR_YUV2BGR_YUYV)
    except: bgr_image = cv2.cvtColor(yuv_matrix, cv2.COLOR_YUV2BGR_UYVY)
    return bgr_image

def handle_client(conn, addr):
    global format_requested_flag, ota_requested_flag
    try:
        conn.settimeout(10.0) # Увеличим таймаут для OTA сессий
        header = conn.recv(12)
        if not header or len(header) < 12:
            return
            
        img_index, img_size, bat_mv = struct.unpack("<III", header)
        
        # Расчет вольтажа батареи (учитываем, что 5.2V в логе — это наводка АЦП без делителя)
        bat_v = bat_mv / 1000.0
        bat_info = f"{bat_v:.2f}V" if bat_mv > 0 else "USB"

        # СЦЕНАРИЙ А: Холостой пинг синхронизации
        if img_size == 0:
            if ota_mode_active := ota_requested_flag:
                if not os.path.exists(OTA_FILE):
                    print(f"[-] Ошибка: Файл {OTA_FILE} не найден в папке сервера!")
                    ota_requested_flag = False
                    conn.close()
                    return
                
                ota_size = os.path.getsize(OTA_FILE)
                conn.sendall(f"OTA:{ota_size}".encode())
                print(f"[!] Отправлен сигнал OTA прошивки ({ota_size} байт) на плату {addr}. Ожидание READY...")
                
                try:
                    resp = conn.recv(5)
                    if b"READY" in resp:
                        print(f"[*] Плата готова. Заливка бинарника {OTA_FILE}...")
                        with open(OTA_FILE, "rb") as f:
                            conn.sendall(f.read())
                        print("[+] Прошивка успешно передана по сети!")
                except Exception as ota_err:
                    print(f"[-] Ошибка во время трансляции OTA: {ota_err}")
                
                ota_requested_flag = False
                conn.close()
                return

            elif format_requested_flag:
                conn.sendall(b"FORMAT_SD\n")
                format_requested_flag = False
                print(f"[!!!] Сигнал FORMAT_SD отправлен на {addr}. Закрываем сокет, плата форматирует карту...")
                conn.close() # Жестко закрываем, предотвращая timeout
                return
            else:
                tx_index = get_max_saved_index()
                conn.sendall(struct.pack("<i", tx_index))
                print(f"[*] Синхронизация: плата {addr} (Батарея: {bat_info}), сервер ждет кадр №{tx_index+1:05d}")
            return

        # СЦЕНАРИЙ Б: Прием кадра
        print(f"[+] Прием кадра №{img_index:05d} ({img_size} байт).")
        
        data_buffer = bytearray()
        while len(data_buffer) < img_size:
            packet = conn.recv(4096)
            if not packet:
                break
            data_buffer.extend(packet)
        
        if len(data_buffer) >= img_size * 0.9: # Допуск 10% на потерю хвоста из-за наводок DMA
            filepath = os.path.join(SAVE_DIR, f"{img_index:05d}.jpg")
            bgr = yuv422_to_jpeg(data_buffer, width=1024, height=768)
            cv2.imwrite(filepath, bgr, [int(cv2.IMWRITE_JPEG_QUALITY), 92])
            conn.sendall(struct.pack("<i", img_index))
            print(f"[+] Кадр {img_index:05d}.jpg успешно сохранен.")
        else:
            print(f"[-] Ошибка: Буфер кадра №{img_index:05d} слишком мал ({len(data_buffer)} байт).")

    except Exception as e:
        pass
    finally:
        try: conn.close()
        except: pass

def console_listener():
    global format_requested_flag, ota_requested_flag
    while True:
        cmd = input().strip().lower()
        if cmd == "format":
            format_requested_flag = True
            print("[!!!] Включён запрос на форматирование флешки на плате!")
        elif cmd == "firmware":
            ota_requested_flag = True
            print(f"[!] Включён запрос на ОБНОВЛЕНИЕ ПО. Файл {OTA_FILE} будет отправлен при следующем коннекте.")

def start_server():
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    
    try:
        server_socket.bind(('0.0.0.0', 8888))
        server_socket.listen(15)
        print(f"[*] TCP Сервер [v2.0] запущен на порту 8888.")
        print(f"[*] Доступные команды: 'format' — очистка SD, 'firmware' — обновление ПО платы по воздуху.")
        
        threading.Thread(target=console_listener, daemon=True).start()
        
        while True:
            conn, addr = server_socket.accept()
            threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()
            
    except KeyboardInterrupt:
        print("\n[*] Перехват KeyboardInterrupt. Корректное закрытие главного сокета...")
        sys.exit(0)
    finally:
        # Решение проблемы блокировки порта при остановке через Ctrl+C
        server_socket.close()
        print("[*] Главный порт 8888 успешно освобожден ОС. Выход.")
        sys.exit(0)

if __name__ == "__main__":
    start_server()
