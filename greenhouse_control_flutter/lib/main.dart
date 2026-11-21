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
  bool _needSyncTimeOnNextConnect = false;
  
  String _statusText = "Статус: Ожидание теплицы...";
  String _batteryText = "Батарея: -- V (--%)";
  String _queueText = "[OK] Железо платы исправно";
  int _totalFramesOnBoard = 0; // Всего кадров в теплице
  int _localFramesCount = 0;   // Сколько уже скачано на телефон
  
  bool _formatRequested = false;
  bool _otaMode = false;
  bool _syncTimeRequested = false; // Триггер ручной корректировки времени
  int? _newIntervalSeconds; // Новый интервал в секундах
  int _currentIntervalSec = 600; // Текущий интервал, полученный с платы
  
  final TextEditingController _dialogController = TextEditingController();

  final TextEditingController _commandController = TextEditingController();
  final GlobalKey _commandFieldKey = GlobalKey();

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
              // Получаем чистое имя файла без пути и без расширения (например, "20261031_161400_00005")
              final name = file.path.split('/').last.split('.').first;
              print("File name = $name");
              
              // НАШ ХАК: Разрезаем имя по символу '_' и забираем самый последний элемент ("00005")
              final indexPart = name.split('_').last;
              
              // Теперь этот кусок гарантированно и чисто распарсится в нормальное число!
              final idx = int.tryParse(indexPart) ?? 0;
              print("File index = $idx");
              
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

  DateTime? _parseDateTimeFromFileName(String fileName) {
    try {
      // Наш формат имени: YYYYMMDD_HHMMSS_INDEX.jpg
      // Пример: 20261101_153022_00001.jpg
      if (fileName.length >= 15) {
        final year = int.parse(fileName.substring(0, 4));
        final month = int.parse(fileName.substring(4, 6));
        final day = int.parse(fileName.substring(6, 8));
        
        final hour = int.parse(fileName.substring(9, 11));
        final minute = int.parse(fileName.substring(11, 13));
        final second = int.parse(fileName.substring(13, 15));
        
        return DateTime(year, month, day, hour, minute, second);
      }
    } catch (e) {
      print("❌ [DATETIME_PARSER] Не удалось распарсить дату из имени файла: $e");
    }
    return null;
  }

  // Универсальный помощник: гарантированно дочитывает N байт из потока и удаляет их из буфера
  Future<Uint8List?> _readExactBytes(
      int count, List<int> buffer, StreamIterator<Uint8List> iterator) async {
    while (buffer.length < count) {
      if (!await iterator.moveNext()) return null;
      buffer.addAll(iterator.current);
    }
    final result = Uint8List.fromList(buffer.sublist(0, count));
    buffer.removeRange(0, count); // Сразу очищаем прочитанное!
    return result;
  }

  // ЛИНЕЙНЫЙ ДИСПЕТЧЕР С ПОШАГОВЫМ ОЖИДАНИЕМ БУФЕРА
  void _handleEsp32Connection(Socket client) async {
    client.timeout(const Duration(seconds: 15)); 
    final iterator = StreamIterator<Uint8List>(client);
    List<int> currentBuffer = [];

    print("🔗 [FLUTTER] Теплица проснулась и подключилась!");

    // ЕСЛИ ФЛАГ ВЗВЕДЕН — АВТОМАТИЧЕСКИ КОРРЕКТИРУЕМ ВРЕМЯ НА ПЛАТЕ!
    if (_needSyncTimeOnNextConnect) {
      final int currentUnixTime = DateTime.now().millisecondsSinceEpoch ~/ 1000;
      final String timeCommand = "TIME:$currentUnixTime";
      
      client.add(Uint8List.fromList(timeCommand.codeUnits));
      await client.flush();
      
      _needSyncTimeOnNextConnect = false; // Сбрасываем флаг, миссия выполнена!
      print("⏰ [FLUTTER] На проснувшуюся плату успешно отправлено точное время: $timeCommand");
    }
      
    // Константы протокола (меняются только здесь!)
    const int baseHeaderInts = 4; // 4 числа = 16 байт (базовый заголовок)
    const int pingExtraInts  = 2; // +2 числа = 8 байт (индекс последнего кадра + интервал сна)

    try {
      // 1. Читаем базовый заголовок (4 числа uint32)
      final baseBytes = await _readExactBytes(baseHeaderInts * 4, currentBuffer, iterator);
      if (baseBytes == null) { await client.close(); iterator.cancel(); return; }

      // Превращаем сырые байты в массив чисел: header[0], header[1], header[2], header[3]
      final header = baseBytes.buffer.asUint32List();
      final int imgIndex    = header[0];
      final int imgSize     = header[1];
      final int batMv       = header[2];
      final int freeSpaceMb = header[3];

      // =======================================================================
      // СЦЕНАРИЙ А: Холостой пинг (imgSize == 0)
      // =======================================================================
      if (imgSize == 0) {
        // Дочитываем хвост пинга (ещё 2 числа uint32)
        final extraBytes = await _readExactBytes(pingExtraInts * 4, currentBuffer, iterator);
        if (extraBytes == null) { await client.close(); iterator.cancel(); return; }

        final extra = extraBytes.buffer.asUint32List();
        final int boardIntervalSec    = extra[0]; // Пятое  число с платы (интервал сна)
        final int lastSavedFrameIndex = extra[1]; // Шестое число с платы

        // Расчёт батареи и ошибок
        double batV = batMv / 1000.0;
        int batPct = (((batV - 3.5) / (4.2 - 3.5)) * 100).clamp(0, 100).toInt();
        double freeGb = freeSpaceMb / 1024.0;

        String hardwareLog = "[OK] Железо платы исправно";
        if (imgIndex > 0) {
          List<String> errorsList = [];
          if ((imgIndex & 0x01) != 0) errorsList.add("Сбой Инит Камеры");
          if ((imgIndex & 0x02) != 0) errorsList.add("Пустой кадр матрицы");
          if ((imgIndex & 0x04) != 0) errorsList.add("Карта SD неисправна");
          hardwareLog = "[Авария]: ${errorsList.join(', ')}";
        }

        int maxSavedIdx = await get_max_saved_index();

        setState(() {
          _queueText = hardwareLog;
          _batteryText = batMv > 0 ? "Батарея: ${batV.toStringAsFixed(2)} V ($batPct%)" : "Батарея: USB";
          _statusText = freeSpaceMb > 0 ? "Карта: ${freeGb.toStringAsFixed(2)} ГБ свободно" : "Карта: Неисправна или Отформатирована";
          _totalFramesOnBoard = lastSavedFrameIndex;
          _localFramesCount = maxSavedIdx;
          if (boardIntervalSec > 0) _currentIntervalSec = boardIntervalSec;
        });

        // --- Очередь ручных команд (строго по приоритету) ---
        bool commandSent = false;

        // Приоритет №1: Время
        if (_syncTimeRequested) {
          final unixSec = DateTime.now().millisecondsSinceEpoch ~/ 1000;
          client.add(Uint8List.fromList("TIME:$unixSec".codeUnits));
          await client.flush();
          _syncTimeRequested = false;
          commandSent = true;
          print("⏰ [FLUTTER] Отправлено время: TIME:$unixSec");
          await Future.delayed(const Duration(milliseconds: 50));
        }

        // Приоритет №2: Форматирование SD
        if (_formatRequested) {
          client.add(Uint8List.fromList("FORMAT_SD".codeUnits));
          await client.flush();
          _formatRequested = false;
          commandSent = true;
          print("💾 [FLUTTER] Отправлен сигнал форматирования SD");
          await Future.delayed(const Duration(milliseconds: 50));
        }

        // Приоритет №2.5: Изменение интервала съёмки (в секундах)
        if (_newIntervalSeconds != null) {
          final int sentInterval = _newIntervalSeconds!; // Запоминаем новое значение
          
          client.add(Uint8List.fromList("INT:$sentInterval".codeUnits));
          await client.flush();
          print("⏱️ [FLUTTER] Отправлен новый интервал: INT:$sentInterval");
          
          setState(() {
            _currentIntervalSec = sentInterval; // Мгновенно обновляем цифру на экране!
            _newIntervalSeconds = null;         // Сбрасываем отложенную команду
          });
          
          commandSent = true;
          await Future.delayed(const Duration(milliseconds: 50));
        }

        // Приоритет №3: Обновление прошивки (строго последней!)
        if (_otaMode) {
          final extDir = await getExternalStorageDirectory();
          if (extDir != null) {
            final otaFile = File('${extDir.path}/firmware.bin');
            if (otaFile.existsSync()) {
              int otaSize = otaFile.lengthSync();
              client.add(Uint8List.fromList("OTA:$otaSize".codeUnits));
              await client.flush();

              // currentBuffer уже очищен от заголовка функцией _readExactBytes,
              // поэтому сразу ждем байты ответа "READY" (82='R', 89='Y')
              while (!currentBuffer.contains(82) || !currentBuffer.contains(89)) {
                if (!await iterator.moveNext()) break;
                currentBuffer.addAll(iterator.current);
              }
              setState(() { _statusText = "Передача прошивки по воздуху..."; });
              final binaryBytes = otaFile.readAsBytesSync();
              client.add(binaryBytes);
              await client.flush();
              _otaMode = false;
              commandSent = true;
              print("🚀 [FLUTTER] Отправлен бинарник OTA прошивки.");
            } else {
              setState(() {
                _statusText = "Ошибка: Файл firmware.bin не найден в папке приложения!";
                _otaMode = false;
              });
            }
          }
        }

        // Всегда отправляем в конце актуальный индекс локального архива (1 число uint32)
        maxSavedIdx = await get_max_saved_index();
        final resp = Uint32List.fromList([maxSavedIdx]);
        client.add(resp.buffer.asUint8List());
        await client.flush();

        if (commandSent) {
          setState(() { _statusText = "Все команды успешно переданы на плату!"; });
        }

        await client.close();
        iterator.cancel();
        return;
      }

      // =======================================================================
      // СЦЕНАРИЙ Б: Прием файла (imgSize > 0)
      // =======================================================================
      // В базовом заголовке для файла третье поле (header[2]) — это длина имени файла!
      final int nameLen = header[2];

      // 1. Читаем имя файла
      final nameBytes = await _readExactBytes(nameLen, currentBuffer, iterator);
      String originalFileName = nameBytes != null ? String.fromCharCodes(nameBytes).trim() : "";
      if (originalFileName.isEmpty) {
        originalFileName = "${imgIndex.toString().padLeft(5, '0')}.jpg";
      }

      // 2. Читаем само тело JPEG
      final jpegBytes = await _readExactBytes(imgSize, currentBuffer, iterator);
      if (jpegBytes == null) { await client.close(); iterator.cancel(); return; }

      // 3. Сохраняем на диск и выставляем дату из имени файла
      try {
        final extDir = await getExternalStorageDirectory();
        if (extDir != null) {
          final greenhouseFolder = Directory('${extDir.path}/greenhouse_archive');
          if (!greenhouseFolder.existsSync()) greenhouseFolder.createSync(recursive: true);

          final file = File('${greenhouseFolder.path}/$originalFileName');
          await file.writeAsBytes(jpegBytes);

          final fileDateTime = _parseDateTimeFromFileName(originalFileName);
          if (fileDateTime != null) {
            await file.setLastModified(fileDateTime);
            print("⏰ [FLUTTER] Системное время файла обновлено на: $fileDateTime");
          }
        }
      } catch (e) { print("Ошибка записи файла: $e"); }

      // 4. Обновляем интерфейс приложения
      setState(() {
        _jpegBytes = jpegBytes;
        _statusText = "Скачан кадр: $originalFileName";
        _localFramesCount = imgIndex;
      });

      // 5. Отправляем плате подтверждение ACK по индексу кадра
      final ack = Uint32List.fromList([imgIndex]);
      client.add(ack.buffer.asUint8List());
      await client.flush();
      await client.close();
      iterator.cancel();
      return;

    } catch (e) {
      setState(() { _statusText = "Сбой сессии: $e"; });
      try { await client.close(); } catch (_) {}
      iterator.cancel();
    }
  }

  void _sendManualCommand(String command) {
    final upperCmd = command.toUpperCase();

    // Проверяем команду на соответствие нашему списку
    if (upperCmd == 'SET_TIME') {
      setState(() {
        _syncTimeRequested = true;
        _statusText = "⏰ Команда SET_TIME отложена до подключения платы...";
      });
      print("⏰ [FLUTTER] Флаг синхронизации времени взведен.");
    } else if (upperCmd == 'FORMAT_SD') {
      setState(() { 
        _formatRequested = true; 
        _statusText = "💾 Команда FORMAT_SD отложена до подключения платы...";
      });
      print("💾 [FLUTTER] Флаг форматирования SD взведен.");
    } else if (upperCmd == 'OTA') {
      setState(() { 
        _otaMode = true; 
        _statusText = "🚀 Режим OTA отложен до подключения платы...";
      });
      print("🚀 [FLUTTER] Режим OTA взведен.");
    } else if (upperCmd.startsWith('INT:')) {
      final val = int.tryParse(upperCmd.substring(4));
      if (val != null && val >= 5 && val <= 604800) {
        setState(() {
          _newIntervalSeconds = val;
          _statusText = "⏱️ Интервал $val сек. отложен до подключения...";
        });
      } else {
        setState(() {
          _statusText = "❌ Ошибка: укажите от 5 до 604800 сек (пример: INT:600)";
        });
      }
    } else {
      // ЕСЛИ КОМАНДА НЕ РАСПОЗНАНА:
      setState(() {
        _statusText = "❌ Ошибка: Неизвестная команда '$command'!";
      });
      print("⚠️ [FLUTTER] Попытка ввода неверной команды: $command");
    }
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
            Text( "⏱️ Интервал съёмки: $_currentIntervalSec сек. (${(_currentIntervalSec / 60).toStringAsFixed(1)} мин.)",
              style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w500),
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

            Padding(
              padding: const EdgeInsets.all(8.0),
              child: Row(
                children: [
                  // === КНОПКА-СПИСОК ДОСТУПНЫХ КОМАНД ===
                        // === БЕЗОПАСНАЯ КНОПКА-ШПАРГАЛКА ===
                  IconButton(
                    icon: const Icon(Icons.help_outline),
                    tooltip: 'Справка по командам',
                    onPressed: () {
                      showDialog(
                        context: context,
                        builder: (BuildContext context) {
                          return AlertDialog(
                            title: const Text('Доступные команды'),
                            content: const SingleChildScrollView(
                              child: ListBody(
                                children: [
                                  Text('SET_TIME', style: TextStyle(fontWeight: FontWeight.bold)),
                                  Text('Синхронизировать часы платы со смартфоном\n'),
                                  
                                  Text('INT:<секунды>', style: TextStyle(fontWeight: FontWeight.bold)),
                                  Text('Интервал съёмки (от 15 до 604800 сек.)\n'),
                                  
                                  Text('FORMAT_SD', style: TextStyle(fontWeight: FontWeight.bold, color: Colors.red)),
                                  Text('Полная очистка SD-карты теплицы\n'),
                                  
                                  Text('OTA', style: TextStyle(fontWeight: FontWeight.bold, color: Colors.orange)),
                                  Text('Обновить прошивку из файла firmware.bin'),
                                ],
                              ),
                            ),
                            actions: [
                              TextButton(
                                child: const Text('Понятно'),
                                onPressed: () => Navigator.of(context).pop(),
                              ),
                            ],
                          );
                        },
                      );
                    },
                  ),

                  // === ВАШЕ ПОЛЕ ВВОДА (БЕЗ ИЗМЕНЕНИЙ) ===
                  Expanded(
                    child: TextField(
                      key: _commandFieldKey,
                      controller: _commandController,
                      textInputAction: TextInputAction.send,
                      onSubmitted: (value) {
                        final cmd = value.trim();
                        if (cmd.isNotEmpty) {
                          _sendManualCommand(cmd);
                          _commandController.clear();
                        }
                        FocusScope.of(context).unfocus();
                      },
                      decoration: const InputDecoration(
                        labelText: 'Команда (нажмите ? для списка)',
                        border: OutlineInputBorder(),
                      ),
                    ),
                  ),
                  const SizedBox(width: 8),

                  // === ВАША КНОПКА ОТПРАВКИ (БЕЗ ИЗМЕНЕНИЙ) ===
                  IconButton(
                    icon: const Icon(Icons.send),
                    onPressed: () {
                      final cmd = _commandController.text.trim();
                      if (cmd.isNotEmpty) {
                        _sendManualCommand(cmd);
                        _commandController.clear();
                      }
                      FocusScope.of(context).unfocus();
                    },
                  ),
                ],
              ),
            )
          ],
        ),
      ),
    );
  }
}
