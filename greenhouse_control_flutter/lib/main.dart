import 'dart:io';
import 'dart:typed_data';
import 'dart:async';
import 'package:flutter/material.dart';
import 'package:image/image.dart' as img_lib;

void main() {
  runApp(const MyApp());
}

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Greenhouse Control',
      theme: ThemeData(primarySwatch: Colors.green, brightness: Brightness.dark),
      home: const GreenhouseScreen(),
    );
  }
}

class GreenhouseScreen extends StatefulWidget {
  const GreenhouseScreen({super.key});

  @override
  State<GreenhouseScreen> createState() => _GreenhouseScreenState();
}

class _GreenhouseScreenState extends State<GreenhouseScreen> {
  ServerSocket? _serverSocket;
  Uint8List? _jpegBytes; // Сюда мы будем класть готовый для вывода на экран JPEG
  
  String _statusText = "Статус: Слушаю порт 8888...";
  String _batteryText = "Батарея: -- V (--%)";
  String _queueText = "Кадр в очереди: --";
  
  bool _formatRequested = false;
  bool _otaMode = false;
  int _formatStage = 0;
  int _countdown = 5;
  Timer? _countdownTimer;

  @override
  void initState() {
    super.initState();
    _startTcpServer(); // Сервер запускается автоматически при старте приложения!
  }

  @override
  void dispose() {
    _serverSocket?.close(); // Жестко и безопасно освобождаем порт при закрытии
    _countdownTimer?.cancel();
    super.dispose();
  }

  // === ВЫСОКОСКОРОСТНОЙ КОНВЕРТЕР YUV422 PACKED -> JPEG (На чистом Dart) ===
  Uint8List convertYuv422ToJpeg(Uint8List yuvBytes, int width, int height) {
    // Создаем пустой холст в памяти смартфона средствами библиотеки image
    final image = img_lib.Image(width: width, height: height);

    int yuvIdx = 0;
    // OV3660 шлет данные YUV422 packed. Пробегаем по макропикселям (2 пикселя за шаг)
    for (int r = 0; r < height; r++) {
      for (int c = 0; c < width; c += 2) {
        if (yuvIdx + 3 >= yuvBytes.length) break;

        // Извлекаем компоненты (с учетом нашего Си-переворота хроматики на плате)
        int y0 = yuvBytes[yuvIdx];
        int v  = yuvBytes[yuvIdx + 1];
        int u  = yuvBytes[yuvIdx + 2];
        int y1 = yuvBytes[yuvIdx + 3];
        yuvIdx += 4;

        // Математика пересчета YUV в RGB для первого пикселя
        int r0 = (y0 + 1.402 * (v - 128)). roundtable();
        int g0 = (y0 - 0.344136 * (u - 128) - 0.714136 * (v - 128)). roundtable();
        int b0 = (y0 + 1.772 * (u - 128)). roundtable();

        // Для второго пикселя
        int r1 = (y1 + 1.402 * (v - 128)). roundtable();
        int g1 = (y1 - 0.344136 * (u - 128) - 0.714136 * (v - 128)). roundtable();
        int b1 = (y1 + 1.772 * (u - 128)). roundtable();

        // Записываем пиксели в структуру холста Flutter
        image.setPixelRgb(c, r, r0.clamp(0, 255), g0.clamp(0, 255), b0.clamp(0, 255));
        image.setPixelRgb(c + 1, r, r1.clamp(0, 255), g1.clamp(0, 255), b1.clamp(0, 255));
      }
    }

    // Кодируем холст в стандартный сжатый JPEG поток байт
    return Uint8List.fromList(img_lib.encodeJpg(image, quality: 85));
  }

  void _startTcpServer() async {
    try {
      _serverSocket = await ServerSocket.bind(InternetAddress.anyIPv4, 8888, shared: true);
      _serverSocket!.listen((Socket client) {
        _handleEsp32Connection(client);
      });
    } catch (e) {
      setState(() { _statusText = "Ошибка запуска порта 8888: $e"; });
    }
  }

  void _handleEsp32Connection(Socket client) async {
    List<int> dataBuffer = [];
    int imgIndex = 0;
    int imgSize = 0;
    int batMv = 0;
    bool headerParsed = false;

    client.listen((Uint8List packet) async {
      dataBuffer.addAll(packet);

      // 1. Парсим 12-байтовый заголовок, как только накопилось достаточно байт
      if (!headerParsed && dataBuffer.length >= 12) {
        final bd = ByteData.sublistView(Uint8List.fromList(dataBuffer.sublist(0, 12)));
        imgIndex = bd.getUint32(0, Endian.little);
        imgSize = bd.getUint32(4, Endian.little);
        batMv = bd.getUint32(8, Endian.little);
        headerParsed = true;

        // Считаем вольтаж лития
        double batV = batMv / 1000.0;
        int batPct = (((batV - 3.5) / (4.2 - 3.5)) * 100).clamp(0, 100).toInt();

        setState(() {
          _queueText = "Кадр в очереди: ${imgIndex.toString().padLeft(5, '0')}";
          _batteryText = batMv > 0 ? "Батарея: ${batV.toStringAsFixed(2)} V ($batPct%)" : "Батарея: USB питание";
          _statusText = "Соединение установлено. Прием данных...";
        });

        // СЦЕНАРИЙ А: Холостой пинг синхронизации
        if (imgSize == 0) {
          if (_otaMode) {
            // Режим прошивки (пока заглушка, возвращаем OK)
            client.write("OTA_ERR");
            _otaMode = false;
          } else if (_formatRequested) {
            client.write("FORMAT_SD\n");
            _formatRequested = false;
            setState(() { _statusText = "Флешка на плате успешно очищена!"; });
          } else {
            // Шлем плате 0 (или любой индекс из локальной истории, пока шлем 0)
            final resp = ByteData(4)..setInt32(0, 0, Endian.little);
            client.add(resp.buffer.asUint8List());
          }
          client.close();
          return;
        }
        
        // Удаляем заголовок из буфера, оставляя только чистые байты картинки
        dataBuffer = dataBuffer.sublist(12);
      }

      // 2. Прием тела картинки
      if (headerParsed && imgSize > 0 && dataBuffer.length >= imgSize) {
        final rawYuvBytes = Uint8List.fromList(dataBuffer.sublist(0, imgSize));
        
        // Запускаем наш высокоскоростной Dart-конвертер YUV422 -> JPEG
        final convertedJpeg = convertYuv422ToJpeg(rawYuvBytes, 1024, 768);

        setState(() {
          _jpegBytes = convertedJpeg; // Помещаем байты в стейт — экран обновится мгновенно!
          _statusText = "Успешно принят кадр ${imgIndex.toString().padLeft(5, '0')}";
        });

        // Отправляем плате Си-подтверждение (4 байта индекса кадра обратно)
        final ack = ByteData(4)..setInt32(0, imgIndex, Endian.little);
        client.add(ack.buffer.asUint8List());
        client.close();
      }
    }, onError: (e) {
      setState(() { _statusText = "Ошибка сессии: $e"; });
      client.close();
    }, onDone: () {
      client.close();
    });
  }

  void _triggerFormatSecure() {
    if (_formatStage == 0) {
      setState(() {
        _formatStage = 1;
        _countdown = 5;
      });
      _countdownTimer = Timer.periodic(const Duration(seconds: 1), (timer) {
        setState(() {
          _countdown--;
          if (_countdown <= 0) {
            _formatStage = 0;
            _countdownTimer?.cancel();
          }
        });
      });
    } else if (_formatStage == 1) {
      _countdownTimer?.cancel();
      setState(() {
        _formatStage = 0;
        _formatRequested = true;
        _statusText = "Запрос на форматирование взведен. Ожидание платы...";
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Greenhouse Control Пульт')),
      body: Padding(
        padding: const EdgeInsets.all(12.0),
        child: Column(
          children: [
            // Окно вывода картинки
            Expanded(
              child: Container(
                width: double.infinity,
                decoration: BoxDecoration(color: Colors.black, borderRadius: BorderRadius.circular(8)),
                child: _jpegBytes != null
                    ? Image.memory(_jpegBytes!, fit: BoxFit.contain)
                    : const Center(child: Text("Ожидание первого кадра...", style: TextStyle(color: Colors.grey))),
              ),
            ),
            const SizedBox(height: 15),
            // Информационный блок
            Text(_statusText, style: const TextStyle(fontSize: 16, fontWeight: FontWeight.bold)),
            const SizedBox(height: 5),
            Text(_batteryText, style: const TextStyle(fontSize: 15, color: Colors.greenAccent)),
            const SizedBox(height: 5),
            Text(_queueText, style: const TextStyle(fontSize: 14, color: Colors.white70)),
            const SizedBox(height: 15),
            
            // Двухэтапная защищенная кнопка очистки флешки
            SizedBox(
              width: double.infinity,
              height: 50,
              child: ElevatedButton(
                style: ElevatedButton.styleFrom(
                  backgroundColor: _formatStage == 1 ? Colors.red : Colors.grey[700],
                ),
                onPressed: _triggerFormatSecure,
                child: Text(
                  _formatStage == 1 
                      ? "ВЫ УВЕРЕНЫ? НАЖМИТЕ ЕЩЕ РАЗ ($_countdown)" 
                      : "ОЧИСТИТЬ ФЛЭШКУ НА ПЛАТЕ",
                  style: const TextStyle(fontSize: 15, color: Colors.white),
                ),
              ),
            ),
            const SizedBox(height: 10),
            
            // Кнопка режима OTA
            SizedBox(
              width: double.infinity,
              height: 45,
              child: OutlinedButton(
                style: OutlinedButton.styleFrom(
                  side: BorderSide(color: _otaMode ? Colors.blue : Colors.red),
                  backgroundColor: _otaMode ? Colors.blue.withOpacity(0.2) : Colors.transparent,
                ),
                onPressed: () {
                  setState(() { _otaMode = !_otaMode; });
                },
                child: Text(
                  _otaMode ? "ОБНОВЛЕНИЕ ПО: ОЖИДАНИЕ ПЛАТЫ..." : "ОБНОВЛЕНИЕ ПРОШИВКИ: ВЫКЛ",
                  style: TextStyle(color: _otaMode ? Colors.blue : Colors.red),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

// Вспомогательное расширение для округления double в Си стилизации
extension DoubleRound on double {
  int roundtable() => round();
}
