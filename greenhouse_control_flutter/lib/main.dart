import 'dart:io';
import 'dart:typed_data';
import 'dart:async';
import 'package:flutter/material.dart';
import 'package:image/image.dart' as img_lib;
import 'package:path_provider/path_provider.dart';

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
  Uint8List? _jpegBytes; 
  
  String _statusText = "Статус: Ожидание теплицы...";
  String _batteryText = "Батарея: -- V (--%)";
  String _queueText = "[OK] Железо платы исправно";
  int _totalFramesOnBoard = 0; // Всего кадров в теплице
  int _localFramesCount = 0;   // Сколько уже скачано на телефон
  
  bool _formatRequested = false;
  bool _otaMode = false;
  
  final TextEditingController _dialogController = TextEditingController();

  @override
  void initState() {
    super.initState();
    _startTcpServer(); 
  }

  @override
  void dispose() {
    _serverSocket?.close(); 
    _dialogController.dispose();
    super.dispose();
  }

  Uint8List convertYuv422ToJpeg(Uint8List yuvBytes, int width, int height) {
    final image = img_lib.Image(width: width, height: height);
    int yuvIdx = 0;
    for (int r = 0; r < height; r++) {
      for (int c = 0; c < width; c += 2) {
        if (yuvIdx + 3 >= yuvBytes.length) break;

        int y0 = yuvBytes[yuvIdx];
        int u  = yuvBytes[yuvIdx + 1]; 
        int y1 = yuvBytes[yuvIdx + 2];
        int v  = yuvBytes[yuvIdx + 3];
        yuvIdx += 4;

        int r0 = (y0 + 1.402 * (v - 128)).round();
        int g0 = (y0 - 0.344136 * (u - 128) - 0.714136 * (v - 128)).round();
        int b0 = (y0 + 1.772 * (u - 128)).round();

        int r1 = (y1 + 1.402 * (v - 128)).round();
        int g1 = (y1 - 0.344136 * (u - 128) - 0.714136 * (v - 128)).round();
        int b1 = (y1 + 1.772 * (u - 128)).round();

        image.setPixelRgb(c, r, r0.clamp(0, 255), g0.clamp(0, 255), b0.clamp(0, 255));
        image.setPixelRgb(c + 1, r, r1.clamp(0, 255), g1.clamp(0, 255), b1.clamp(0, 255));
      }
    }
    return Uint8List.fromList(img_lib.encodeJpg(image, quality: 100));
  }

  // РЕШЕНИЕ ПРОБЛЕМЫ СИНХРОНИЗАЦИИ: Сканируем папку по динамическому пути песочницы!
  Future<int> get_max_saved_index() async {
    int maxIndex = 0;
    try {
      final extDir = await getExternalStorageDirectory();
      if (extDir != null) {
        final rootDir = Directory('${extDir.path}/greenhouse_archive');
        if (await rootDir.exists()) {
          final files = rootDir.listSync();
          for (var file in files) {
            if (file is File && file.path.endsWith(".jpg")) {
              final name = file.path.split('/').last.split('.').first;
              final idx = int.tryParse(name) ?? 0;
              if (idx > maxIndex) maxIndex = idx;
            }
          }
        }
      }
    } catch (_) {}
    print("[*] Сервер проверил диск. Максимальный индекс: $maxIndex");
    return maxIndex;
  }

  void _startTcpServer() async {
    try {
      _serverSocket = await ServerSocket.bind(InternetAddress.anyIPv4, 8888, shared: true);
      _serverSocket!.listen((Socket client) {
        _handleEsp32Connection(client);
      });
    } catch (e) {
      setState(() { _statusText = "Ошибка порта 8888: $e"; });
    }
  }

  // ЛИНЕЙНЫЙ ДИСПЕТЧЕР С ПОШАГОВЫМ ОЖИДАНИЕМ БУФЕРА
   void _handleEsp32Connection(Socket client) async {
    client.timeout(const Duration(seconds: 15)); 
    final iterator = StreamIterator<Uint8List>(client);
    List<int> currentBuffer = [];

    try {
      // Шаг 1: Сначала всегда читаем стандартные 16 байт заголовка
      while (currentBuffer.length < 16) {
        if (!await iterator.moveNext()) break;
        currentBuffer.addAll(iterator.current);
      }

      if (currentBuffer.length < 16) {
        await client.close(); iterator.cancel(); return;
      }

      final bd = ByteData.sublistView(Uint8List.fromList(currentBuffer.sublist(0, 16)));
      int imgIndex = bd.getUint32(0, Endian.little);
      int imgSize = bd.getUint32(4, Endian.little);
      int batMv = bd.getUint32(8, Endian.little);
      int freeSpaceMb = bd.getUint32(12, Endian.little);

      double batV = batMv / 1000.0;
      int batPct = (((batV - 3.5) / (4.2 - 3.5)) * 100).clamp(0, 100).toInt();
      double freeGb = freeSpaceMb / 1024.0;

      // =======================================================================
      // СЦЕНАРИЙ А: Холостой пинг (ESP32 прислала нам 20 байт)
      // =======================================================================
      if (imgSize == 0) {
        // ДОЧИТЫВАЕМ ЕЩЕ 4 БАЙТА (пятый элемент header[4] из ESP32)
        while (currentBuffer.length < 20) {
          if (!await iterator.moveNext()) break;
          currentBuffer.addAll(iterator.current);
        }

        if (currentBuffer.length < 20) {
          await client.close(); iterator.cancel(); return;
        }

        // Парсим наше новое 5-е поле (смещение 16 байт от начала заголовка)
        final extendedBd = ByteData.sublistView(Uint8List.fromList(currentBuffer.sublist(16, 20)));
        int lastSavedFrameIndex = extendedBd.getUint32(0, Endian.little);

        int errorMask = imgIndex; 
        String hardwareLog = "[OK] Железо платы исправно";
        if (errorMask > 0) {
          List<String> errorsList = [];
          if ((errorMask & 0x01) != 0) errorsList.add("Сбой Инит Камеры");
          if ((errorMask & 0x02) != 0) errorsList.add("Пустой кадр матрицы");
          if ((errorMask & 0x04) != 0) errorsList.add("Карта SD неисправна");
          hardwareLog = "[Авария]: ${errorsList.join(', ')}";
        }

        String storageStr = freeSpaceMb > 0 ? "Карта: ${freeGb.toStringAsFixed(2)} ГБ свободно" : "Карта: Неисправна или Отформатирована";
        
        // Переменная для вывода информации о количестве кадров на экране смартфона
        String frameCounterStr = "Всего кадров в теплице: $lastSavedFrameIndex";

        // Узнаем, сколько у нас уже скачано локально
int maxSavedIdx = await get_max_saved_index();

setState(() {
  _queueText = hardwareLog; 
  _batteryText = batMv > 0 ? "Батарея: ${batV.toStringAsFixed(2)} V ($batPct%)" : "Батарея: USB";
  _statusText = storageStr;
  
  // Сохраняем значения в глобальные переменные состояния класса!
  _totalFramesOnBoard = lastSavedFrameIndex;
  _localFramesCount = maxSavedIdx; 
});

        if (_otaMode) {
          // 1. Получаем путь к системной папке приложения динамически
          final extDir = await getExternalStorageDirectory();
          
          if (extDir != null) {
            // 2. Формируем правильный путь к файлу firmware.bin в этой папке
            final otaFile = File('${extDir.path}/firmware.bin');

            if (otaFile.existsSync()) {
              int otaSize = otaFile.lengthSync();
              client.add(Uint8List.fromList("OTA:$otaSize".codeUnits));
              await client.flush(); 
              
              currentBuffer = currentBuffer.sublist(20);
              while (!currentBuffer.contains(82) || !currentBuffer.contains(89)) { 
                if (!await iterator.moveNext()) break;
                currentBuffer.addAll(iterator.current);
              }
              setState(() { _statusText = "Передача прошивки по воздуху..."; });
              final binaryBytes = otaFile.readAsBytesSync();
              client.add(binaryBytes);
              await client.flush(); 
              setState(() { _otaMode = false; _statusText = "Прошивка успешно загружена!"; });
            } else {
              // Маленький бонус: если режима OTA включен, но файла нет, выводим подсказку
              setState(() { _statusText = "Ошибка: Файл firmware.bin не найден в папке приложения!"; _otaMode = false; });
            }
          }
          
          await client.close(); iterator.cancel(); return;
        } else if (_formatRequested) {
          client.add(Uint8List.fromList("FORMAT_SD\n".codeUnits)); await client.flush();
          _formatRequested = false;
          setState(() { _statusText = "Сигнал форматирования передан!"; });
          await client.close(); iterator.cancel(); return;
        } else {
          int maxSavedIdx = await get_max_saved_index();
          final resp = ByteData(4)..setInt32(0, maxSavedIdx, Endian.little);
          client.add(resp.buffer.asUint8List());
          await client.flush(); await client.close(); iterator.cancel(); return;
        }
      }

      // =======================================================================
      // СЦЕНАРИЙ Б: Прием файла (ESP32 прислала строго 16 байт заголовка)
      // =======================================================================
      if (imgSize > 0) {
        // Отрезаем ровно 16 байт заголовка, всё остальное — это байты нашей картинки
        List<int> imagePayload = currentBuffer.sublist(16);
        while (imagePayload.length < imgSize) {
          if (!await iterator.moveNext()) break;
          imagePayload.addAll(iterator.current);
        }

        final rawYuvBytes = Uint8List.fromList(imagePayload.sublist(0, imgSize));
        
        final convertedJpeg = convertYuv422ToJpeg(rawYuvBytes, 1280, 1024);

        try {
          final extDir = await getExternalStorageDirectory();
          if (extDir != null) {
            final greenhouseFolder = Directory('${extDir.path}/greenhouse_archive');
            if (!greenhouseFolder.existsSync()) greenhouseFolder.createSync(recursive: true);
            
            final fileStringIndex = imgIndex.toString().padLeft(5, '0');
            final file = File('${greenhouseFolder.path}/$fileStringIndex.jpg');
            await file.writeAsBytes(convertedJpeg); 
          }
        } catch (e) { print("Ошибка записи: $e"); }

        // Считаем прогресс: текущий индекс кадра делим на общее число сохраненных кадров
        // Здесь вы можете использовать информацию для обновления UI-прогрессбара
        setState(() {
          _jpegBytes = convertedJpeg;
          _statusText = "Скачан кадр №${imgIndex.toString().padLeft(5, '0')}";
          
          // Ме мега-удобно инкрементируем наш счетчик скачанных файлов прямо на лету!
          _localFramesCount = imgIndex; 
        });
        

        final ack = ByteData(4)..setInt32(0, imgIndex, Endian.little);
        client.add(ack.buffer.asUint8List());
        await client.flush(); await client.close(); iterator.cancel();
      }

    } catch (e) {
      setState(() { _statusText = "Сбой сессии: $e"; });
      try { await client.close(); } catch (_) {}
      iterator.cancel();
    }
  }

  void _showFormatDialog() {
    _dialogController.clear();
    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text("Очистка флешки теплицы"),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Text("Эта операция полностью сотрет все файлы на SD-карте платы. Данное действие необратимо!"),
            const SizedBox(height: 15),
            TextField(
              controller: _dialogController,
              decoration: const InputDecoration(labelText: "Введите проверочное слово 'format'"),
            )
          ],
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text("ОТМЕНА")),
          ElevatedButton(
            style: ElevatedButton.styleFrom(backgroundColor: Colors.red),
            onPressed: () {
              if (_dialogController.text.trim().toLowerCase() == "format") {
                setState(() { _formatRequested = true; _statusText = "Запрос очистки. Ожидание платы..."; });
                Navigator.pop(context);
              }
            },
            child: const Text("СТЕРЕТЬ ВСЁ"),
          )
        ],
      ),
    );
  }

  void _showOtaDialog() {
    _dialogController.clear();
    showDialog(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text("Обновление прошивки по воздуху"),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            const Text("Скопируйте файл 'firmware.bin' с помощью Cx Проводника в папку приложения: Android/data/com.example.greenhouse_control_flutter/files/greenhouse_archive/"),
            const SizedBox(height: 15),
            TextField(
              controller: _dialogController,
              decoration: const InputDecoration(labelText: "Введите проверочное слово 'firmware'"),
            )
          ],
        ),

        actions: [
          TextButton(onPressed: () => Navigator.pop(context), child: const Text("ОТМЕНА")),
          ElevatedButton(
            style: ElevatedButton.styleFrom(backgroundColor: Colors.blue),
            onPressed: () {
            if (_dialogController.text.trim().toLowerCase() == "firmware") {
              setState(() { _otaMode = true; _statusText = "Режим OTA активен. Ожидание платы..."; });
              Navigator.pop(context);
            }
          },
          child: const Text("ОБНОВИТЬ ПО"),
        )
      ],
    ),
  );
}

@override
Widget build(BuildContext context) {
  // Вычисляем, сколько кадров осталось скачать
  int framesLeft = _totalFramesOnBoard - _localFramesCount;
  if (framesLeft < 0) framesLeft = 0;

  return Scaffold(
    appBar: AppBar(title: const Text('Greenhouse Control Пульт')),
    body: Padding(
      padding: const EdgeInsets.all(12.0),
      child: Column(
        children: [
          Expanded(
            child: Container(
              width: double.infinity,
              decoration: BoxDecoration(color: Colors.black, borderRadius: BorderRadius.circular(8)),
              child: _jpegBytes != null
                  ? Image.memory(_jpegBytes!, fit: BoxFit.contain)
                  : const Center(child: Text("Ожидание теплицы...", style: TextStyle(color: Colors.grey))),
            ),
          ),
          const SizedBox(height: 15),
          Text(_statusText, style: const TextStyle(fontSize: 14, fontWeight: FontWeight.bold), textAlign: TextAlign.center),
          const SizedBox(height: 5),
          Text(_queueText, style: TextStyle(fontSize: 15, color: _queueText.contains("Авария") ? Colors.redAccent : Colors.white70, fontWeight: _queueText.contains("Аvaрия") ? FontWeight.bold : FontWeight.normal)),
          const SizedBox(height: 5),
          Text(_batteryText, style: const TextStyle(fontSize: 15, color: Colors.greenAccent)),
          
          // =======================================================================
          // НАШ НОВЫЙ БЛОК: ИНДИКАТОР ОЧЕРЕДИ СКАЧИВАНИЯ КАДРОВ
          // =======================================================================
          const SizedBox(height: 10),
          Container(
            padding: const EdgeInsets.symmetric(vertical: 8, horizontal: 12),
            decoration: BoxDecoration(
              color: framesLeft > 0 ? Colors.blue.withOpacity(0.1) : Colors.green.withOpacity(0.1),
              borderRadius: BorderRadius.circular(6),
            ),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.spaceBetween,
              children: [
                Text(
                  framesLeft > 0 ? "Осталось скачать: $framesLeft кадров" : "Синхронизировано",
                  style: TextStyle(
                    fontSize: 14, 
                    fontWeight: FontWeight.bold, 
                    color: framesLeft > 0 ? Colors.blue : Colors.greenAccent
                  ),
                ),
                Text(
                  "$_localFramesCount из $_totalFramesOnBoard",
                  style: const TextStyle(fontSize: 14, color: Colors.grey),
                ),
              ],
            ),
          ),
          // =======================================================================

          const SizedBox(height: 15),
          SizedBox(
            width: double.infinity,
            height: 48,
            child: ElevatedButton(
              style: ElevatedButton.styleFrom(backgroundColor: Colors.grey),
              onPressed: _showFormatDialog,
              child: const Text("ОЧИСТИТЬ ФЛЭШКУ НА ПЛАТЕ", style: TextStyle(fontSize: 14, color: Colors.white)),
            ),
          ),
          const SizedBox(height: 10),
          SizedBox(
            width: double.infinity,
            height: 44,
            child: OutlinedButton(
              style: OutlinedButton.styleFrom(
                side: BorderSide(color: _otaMode ? Colors.blue : Colors.redAccent),
                backgroundColor: _otaMode ? Colors.blue.withOpacity(0.1) : Colors.transparent,
              ),
              onPressed: _showOtaDialog,
              child: Text(
                _otaMode ? "ОБНОВЛЕНИЕ ПО: В ЖДУЩЕМ РЕЖИМЕ..." : "ОБНОВИТЬ ПРОШИВКУ (OTA)",
                style: TextStyle(color: _otaMode ? Colors.blue : Colors.redAccent, fontWeight: FontWeight.bold),
              ),
            ),
          ),
        ],
      ),
    ),
  );
}
}
