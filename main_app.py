#!/usr/bin/python

import socket
import os
import struct
import cv2
import numpy as np
import threading
from kivy.app import App
from kivy.uix.boxlayout import BoxLayout
from kivy.uix.button import Button
from kivy.uix.label import Label
from kivy.uix.image import Image
from kivy.uix.togglebutton import ToggleButton
from kivy.clock import Clock
from kivy.core.window import Window

Window.size = (360, 640)

class GreenhouseControllerApp(App):
    def build(self):
        self.title = "Greenhouse Control"
        self.server_running = False
        self.ota_mode = False          # Возвращаем флаг OTA-режима
        self.format_requested = False  
        self.server_socket = None
        self.save_dir = "./greenhouse_images"
        self.ota_file_path = "./firmware.bin" # Сюда кладем файл новой прошивки ESP32
        
        self.format_confirm_stage = 0
        self.format_countdown = 5
        self.countdown_event = None

        if not os.path.exists(self.save_dir):
            os.makedirs(self.save_dir)

        layout = BoxLayout(orientation='vertical', padding=10, spacing=10)

        # 1. Виджет вывода изображения
        self.img_widget = Image(source='placeholder.png', allow_stretch=True)
        layout.add_widget(self.img_widget)

        # 2. Информационные лейблы
        self.lbl_status = Label(text="Статус: Инициализация сервера...", size_hint_y=0.08)
        self.lbl_battery = Label(text="Батарея: -- V (--%)", size_hint_y=0.06, color=(0.2, 0.8, 0.2, 1))
        self.lbl_queue = Label(text="Кадр в очереди: --", size_hint_y=0.06)
        layout.add_widget(self.lbl_status)
        layout.add_widget(self.lbl_battery)
        layout.add_widget(self.lbl_queue)

        # 3. Защищенная кнопка очистки памяти
        self.btn_format = Button(text="ОЧИСТИТЬ ФЛЭШКУ НА ПЛАТЕ", size_hint_y=0.1, background_color=(0.5, 0.4, 0.4, 1))
        self.btn_format.bind(on_press=self.trigger_format_secure)
        layout.add_widget(self.btn_format)

        # 4. ВОЗВРАЩАЕМ КНОПКУ ОБНОВЛЕНИЯ ПО (OTA)
        self.btn_ota = ToggleButton(text="ОБНОВЛЕНИЕ ПРОШИВКИ: ВЫКЛ", size_hint_y=0.1, background_color=(0.7, 0.1, 0.1, 1))
        self.btn_ota.bind(on_press=self.toggle_ota)
        layout.add_widget(self.btn_ota)

        return layout

    def on_start(self):
        """Сервер запускается автоматически при старте приложения"""
        self.server_running = True
        self.lbl_status.text = "Статус: Слушаю порт 8888..."
        threading.Thread(target=self.start_tcp_server, daemon=True).start()

    def on_stop(self):
        """Освобождаем порт 8888 при закрытии приложения"""
        self.server_running = False
        if self.server_socket:
            try:
                self.server_socket.shutdown(socket.SHUT_RDWR)
                self.server_socket.close()
            except:
                pass
        print("[*] Сервер остановлен, порт 8888 освобожден.")

    def toggle_ota(self, instance):
        """Логика активации режима беспроводной прошивки"""
        if instance.state == 'down':
            if not os.path.exists(self.ota_file_path):
                self.lbl_status.text = f"Ошибка: {self.ota_file_path} не найден!"
                instance.state = 'normal'
                return
            self.ota_mode = True
            instance.text = "ОБНОВЛЕНИЕ ПРОШИВКИ: ОЖИДАНИЕ ПЛАТЫ..."
            instance.background_color = (0.1, 0.5, 0.8, 1) # Синий цвет режима прошивки
        else:
            self.ota_mode = False
            instance.text = "ОБНОВЛЕНИЕ ПРОШИВКИ: ВЫКЛ"
            instance.background_color = (0.7, 0.1, 0.1, 1)

    def trigger_format_secure(self, instance):
        if self.format_confirm_stage == 0:
            self.format_confirm_stage = 1
            self.format_countdown = 5
            self.btn_format.background_color = (0.9, 0.1, 0.1, 1)
            self.btn_format.text = f"ВЫ УВЕРЕНЫ? НАЖМИТЕ ЕЩЕ РАЗ ({self.format_countdown})"
            self.countdown_event = Clock.schedule_interval(self.update_format_countdown, 1.0)
        elif self.format_confirm_stage == 1:
            self.format_confirm_stage = 0
            if self.countdown_event:
                self.countdown_event.cancel()
            self.format_requested = True 
            self.btn_format.background_color = (0.3, 0.3, 0.3, 1)
            self.btn_format.text = "КОМАНДА ОЧИСТКИ ОТПРАВЛЕНА"
            self.lbl_status.text = "Запрос на форматирование взведен. Ожидание платы..."

    def update_format_countdown(self, dt):
        self.format_countdown -= 1
        if self.format_countdown <= 0:
            self.format_confirm_stage = 0
            self.countdown_event.cancel()
            self.btn_format.background_color = (0.5, 0.4, 0.4, 1)
            self.btn_format.text = "ОЧИСТИТЬ ФЛЭШКУ НА ПЛАТЕ"
        else:
            self.btn_format.text = f"ВЫ УВЕРЕНЫ? НАЖМИТЕ ЕЩЕ РАЗ ({self.format_countdown})"

    def get_max_saved_index(self):
        max_index = 0
        try:
            for filename in os.listdir(self.save_dir):
                if filename.endswith(".jpg"):
                    try:
                        pure_name = filename.split(".")
                        idx = int(pure_name[0]) # Исправлено: берем первый элемент списка строк
                        if idx > max_index: max_index = idx
                    except: continue
        except: pass
        return max_index

    def yuv422_to_jpeg(self, yuv_bytes, width=1024, height=768):
        raw_array = np.frombuffer(yuv_bytes, dtype=np.uint8)[:width*height*2]
        yuv_matrix = raw_array.reshape((height, width, 2))
        try: bgr = cv2.cvtColor(yuv_matrix, cv2.COLOR_YUV2BGR_YUYV)
        except: bgr = cv2.cvtColor(yuv_matrix, cv2.COLOR_YUV2BGR_UYVY)
        return bgr

    def update_ui_elements(self, img_path=None, battery_txt=None, queue_txt=None, status_txt=None):
        if img_path: 
            self.img_widget.source = img_path
            self.img_widget.reload()
        if battery_txt: self.lbl_battery.text = battery_txt
        if queue_txt: self.lbl_queue.text = queue_txt
        if status_txt: self.lbl_status.text = status_txt

    def start_tcp_server(self):
        self.server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.server_socket.bind(('0.0.0.0', 8888))
            self.server_socket.listen(10)
        except Exception as err:
            error_msg = f"Ошибка порта: {err}"
            Clock.schedule_once(lambda dt: self.update_ui_elements(status_txt=error_msg))
            return

        while self.server_running:
            try:
                conn, addr = self.server_socket.accept()
                threading.Thread(target=self.handle_client, args=(conn, addr), daemon=True).start()
            except: break

    def handle_client(self, conn, addr):
        try:
            conn.settimeout(6.0)
            header = conn.recv(12)
                
            img_index, img_size, bat_mv = struct.unpack("<III", header)
            
            bat_v = bat_mv / 1000.0
            if bat_mv > 0:
                bat_pct = int(max(0, min(100, (bat_v - 3.5) / (4.2 - 3.5) * 100)))
                bat_str = f"Батарея: {bat_v:.2f} V ({bat_pct}%)"
            else:
                bat_str = "Батарея: внешнее питание (USB)"

            msg_queue = f"Кадр в очереди: {img_index:05d}"
            Clock.schedule_once(lambda dt: self.update_ui_elements(battery_txt=bat_str, queue_txt=msg_queue, status_txt="Соединение установлено..."))

            # СЦЕНАРИЙ А: Холостой пинг синхронизации очереди
            if img_size == 0:
                if self.ota_mode:
                    # Если пользователь нажал кнопку прошивки, шлем команду "OTA:размер"
                    file_size = os.path.getsize(self.ota_file_path)
                    conn.sendall(f"OTA:{file_size}".encode())
                    
                    # Ожидаем ответ READY от Си-кода платы
                    ready_resp = conn.recv(5)
                    if b"READY" in ready_resp:
                        Clock.schedule_once(lambda dt: self.update_ui_elements(status_txt="Передача бинарника OTA..."))
                        with open(self.ota_file_path, "rb") as f:
                            conn.sendall(f.read())
                        self.ota_mode = False
                        
                        # Возвращаем кнопку в исходный вид
                        Clock.schedule_once(lambda dt: setattr(self.btn_ota, 'state', 'normal'))
                        Clock.schedule_once(lambda dt: setattr(self.btn_ota, 'text', 'ОБНОВЛЕНИЕ ПРОШИВКИ: ВЫКЛ'))
                        Clock.schedule_once(lambda dt: setattr(self.btn_ota, 'background_color', (0.7, 0.1, 0.1, 1)))
                        Clock.schedule_once(lambda dt: self.update_ui_elements(status_txt="Прошивка успешно залита! Плата перезагружается."))
                elif self.format_requested:
                    conn.sendall(b"FORMAT_SD\n")
                    self.format_requested = False
                    Clock.schedule_once(lambda dt: setattr(self.btn_format, 'text', 'ОЧИСТИТЬ ФЛЭШКУ НА ПЛАТЕ'))
                    Clock.schedule_once(lambda dt: setattr(self.btn_format, 'background_color', (0.5, 0.4, 0.4, 1)))
                    Clock.schedule_once(lambda dt: self.update_ui_elements(status_txt="Флешка на плате успешно очищена!"))
                else:
                    tx_index = self.get_max_saved_index()
                    conn.sendall(struct.pack("<i", tx_index))
                return

            # СЦЕНАРИЙ Б: Прием кадра YUV422
            data_buffer = bytearray()
            while len(data_buffer) < img_size:
                packet = conn.recv(4096)
                if not packet: break
                data_buffer.extend(packet)

                if len(data_buffer) == img_size:
                    filepath = os.path.join(self.save_dir, f"{img_index:05d}.jpg")
                    bgr = self.yuv422_to_jpeg(data_buffer, width=1024, height=768)
                    cv2.imwrite(filepath, bgr, [int(cv2.IMWRITE_JPEG_QUALITY), 92])
                    conn.sendall(struct.pack("<i", img_index))
                    success_msg = f"Успешно сохранен кадр {img_index:05d}.jpg"
                    Clock.schedule_once(lambda dt: self.update_ui_elements(img_path=filepath, status_txt=success_msg))
                else:
                    Clock.schedule_once(lambda dt: self.update_ui_elements(status_txt=f"Ошибка: кадр {img_index} оборван"))
        except Exception as e:
            err_session = f"Ошибка сессии: {e}"
            Clock.schedule_once(lambda dt: self.update_ui_elements(status_txt=err_session))
        finally:
            try:
                conn.close() # Закрываем строго один раз здесь
            except:
                pass
        
if __name__ == '__main__':
    GreenhouseControllerApp().run()
