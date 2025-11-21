#include <driver/i2s.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <WebServer.h>

// WiFi Configuration
const char* ssid = "hh00";
const char* password = "helpingthe";

// I2S Configuration
#define I2S_WS 25
#define I2S_SD 33
#define I2S_SCK 32
#define I2S_PORT I2S_NUM_0

// Recording Configuration
#define SAMPLE_RATE 16000
#define BITS_PER_SAMPLE 16
#define CHANNELS 1
#define RECORD_TIME 5
#define BUFFER_SIZE 512
#define WAKE_THRESHOLD 2000  // ปรับค่านี้ตามความดังของเสียง

struct WAVHeader {
  char riff[4] = {'R', 'I', 'F', 'F'};
  uint32_t fileSize;
  char wave[4] = {'W', 'A', 'V', 'E'};
  char fmt[4] = {'f', 'm', 't', ' '};
  uint32_t fmtSize = 16;
  uint16_t audioFormat = 1;
  uint16_t numChannels = CHANNELS;
  uint32_t sampleRate = SAMPLE_RATE;
  uint32_t byteRate = SAMPLE_RATE * CHANNELS * (BITS_PER_SAMPLE / 8);
  uint16_t blockAlign = CHANNELS * (BITS_PER_SAMPLE / 8);
  uint16_t bitsPerSample = BITS_PER_SAMPLE;
  char data[4] = {'d', 'a', 't', 'a'};
  uint32_t dataSize;
};

WebServer server(80);
int16_t buffer[BUFFER_SIZE];
bool isRecording = false;
int fileCounter = 0;

void setupI2S() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = BUFFER_SIZE,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_zero_dma_buffer(I2S_PORT);
  
  Serial.println("✓ I2S initialized");
}

void writeWAVHeader(File file, uint32_t dataSize) {
  WAVHeader header;
  header.dataSize = dataSize;
  header.fileSize = dataSize + sizeof(WAVHeader) - 8;
  
  file.write((uint8_t*)&header, sizeof(WAVHeader));
}

void recordAudio(const char* filename) {
  File audioFile = SPIFFS.open(filename, FILE_WRITE);
  if (!audioFile) {
    Serial.println("❌ Failed to open file for writing");
    return;
  }

  Serial.println("🔴 Recording started...");
  
  writeWAVHeader(audioFile, 0);
  
  uint32_t totalBytesWritten = 0;
  uint32_t recordingDuration = RECORD_TIME * 1000;
  uint32_t startTime = millis();
  
  while (millis() - startTime < recordingDuration) {
    size_t bytesRead = 0;
    i2s_read(I2S_PORT, buffer, BUFFER_SIZE * sizeof(int16_t), &bytesRead, portMAX_DELAY);
    
    audioFile.write((uint8_t*)buffer, bytesRead);
    totalBytesWritten += bytesRead;
    
    if ((millis() - startTime) % 1000 < 50) {
      Serial.print(".");
    }
  }
  
  Serial.println("\n✓ Recording complete!");
  
  audioFile.seek(0);
  writeWAVHeader(audioFile, totalBytesWritten);
  audioFile.close();
  
  Serial.printf("💾 Saved: %s (%d KB)\n", filename, (totalBytesWritten + sizeof(WAVHeader)) / 1024);
  Serial.printf("📊 SPIFFS: %d / %d bytes used\n", SPIFFS.usedBytes(), SPIFFS.totalBytes());
}

bool detectWakeWord() {
  size_t bytesRead = 0;
  i2s_read(I2S_PORT, buffer, BUFFER_SIZE * sizeof(int16_t), &bytesRead, portMAX_DELAY);
  
  int32_t sum = 0;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    sum += abs(buffer[i]);
  }
  int32_t avgVolume = sum / BUFFER_SIZE;
  
  // แสดงระดับเสียงเพื่อช่วย debug
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 2000) {
    Serial.printf("🎤 Volume: %d (Threshold: %d)\n", avgVolume, WAKE_THRESHOLD);
    lastPrint = millis();
  }
  
  if (avgVolume > WAKE_THRESHOLD) {
    Serial.printf("🎯 Wake word detected! Volume: %d\n", avgVolume);
    return true;
  }
  
  return false;
}

void deleteOldestFile() {
  if (SPIFFS.usedBytes() > SPIFFS.totalBytes() * 0.8) {
    Serial.println("⚠️ Storage almost full, deleting oldest...");
    
    char oldestFile[32];
    sprintf(oldestFile, "/rec_000.wav");
    
    if (SPIFFS.exists(oldestFile)) {
      SPIFFS.remove(oldestFile);
      Serial.printf("🗑️ Deleted: %s\n", oldestFile);
      
      for (int i = 1; i < fileCounter; i++) {
        char oldName[32], newName[32];
        sprintf(oldName, "/rec_%03d.wav", i);
        sprintf(newName, "/rec_%03d.wav", i - 1);
        
        if (SPIFFS.exists(oldName)) {
          SPIFFS.rename(oldName, newName);
        }
      }
      fileCounter--;
    }
  }
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>ESP32 Voice Recorder</title>";
  html += "<style>";
  html += "body { font-family: Arial; margin: 20px; background: #f0f0f0; }";
  html += ".container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "h1 { color: #333; text-align: center; }";
  html += ".status { background: #e3f2fd; padding: 15px; border-radius: 5px; margin: 20px 0; }";
  html += ".file-list { margin: 20px 0; }";
  html += ".file-item { background: #f5f5f5; padding: 12px; margin: 8px 0; border-radius: 5px; display: flex; justify-content: space-between; align-items: center; flex-wrap: wrap; }";
  html += ".btn { padding: 8px 16px; margin: 5px; border: none; border-radius: 5px; cursor: pointer; text-decoration: none; display: inline-block; color: white; }";
  html += ".btn-download { background: #4CAF50; }";
  html += ".btn-delete { background: #f44336; }";
  html += ".btn-play { background: #2196F3; }";
  html += ".btn:hover { opacity: 0.8; }";
  html += ".controls { text-align: center; margin: 20px 0; }";
  html += ".info { color: #666; font-size: 14px; }";
  html += "@media (max-width: 600px) { .file-item { flex-direction: column; align-items: flex-start; } }";
  html += "</style></head><body>";
  
  html += "<div class='container'>";
  html += "<h1>🎤 ESP32 Voice Recorder</h1>";
  
  html += "<div class='status'>";
  html += "<strong>📊 Storage:</strong> " + String(SPIFFS.usedBytes()) + " / " + String(SPIFFS.totalBytes()) + " bytes<br>";
  html += "<strong>📁 Files:</strong> " + String(fileCounter) + " recordings<br>";
  html += "<strong>🌐 IP:</strong> " + WiFi.localIP().toString();
  html += "</div>";
  
  html += "<h2>📂 Recordings</h2>";
  html += "<div class='file-list'>";
  
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  bool hasFiles = false;
  
  while (file) {
    String filename = String(file.name());
    if (filename.endsWith(".wav")) {
      hasFiles = true;
      int fileSize = file.size();
      
      html += "<div class='file-item'>";
      html += "<div><strong>" + filename + "</strong><br>";
      html += "<span class='info'>" + String(fileSize / 1024) + " KB | " + String(RECORD_TIME) + "s</span></div>";
      html += "<div>";
      html += "<a href='/download?file=" + filename + "' class='btn btn-download'>⬇️ Download</a>";
      html += "<a href='/play?file=" + filename + "' class='btn btn-play' target='_blank'>▶️ Play</a>";
      html += "<a href='/delete?file=" + filename + "' class='btn btn-delete' onclick='return confirm(\"Delete this file?\")'>🗑️ Delete</a>";
      html += "</div></div>";
    }
    file = root.openNextFile();
  }
  
  if (!hasFiles) {
    html += "<p style='text-align:center; color:#999; padding: 30px;'>ยังไม่มีไฟล์บันทึก<br>พูดว่า 'Jarvis' เพื่อเริ่มบันทึกเสียง!</p>";
  }
  
  html += "</div>";
  
  html += "<div class='controls'>";
  html += "<a href='/deleteall' class='btn btn-delete' onclick='return confirm(\"ลบไฟล์ทั้งหมด?\")'>🗑️ Delete All</a>";
  html += "<a href='/' class='btn btn-play'>🔄 Refresh</a>";
  html += "</div>";
  
  html += "<p style='text-align:center; color:#999; margin-top:30px;'>💡 พูดว่า 'Jarvis' ดังๆ เพื่อเริ่มบันทึก " + String(RECORD_TIME) + " วินาที</p>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleDownload() {
  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "Missing file parameter");
    return;
  }
  
  String filename = server.arg("file");
  
  // เพิ่ม "/" ถ้ายังไม่มี
  if (!filename.startsWith("/")) {
    filename = "/" + filename;
  }
  
  Serial.println("Download request: " + filename);
  
  if (!SPIFFS.exists(filename)) {
    Serial.println("❌ File not found: " + filename);
    server.send(404, "text/plain", "File not found: " + filename);
    return;
  }
  
  File file = SPIFFS.open(filename, "r");
  if (!file) {
    Serial.println("❌ Failed to open file");
    server.send(500, "text/plain", "Failed to open file");
    return;
  }
  
  Serial.println("✓ Sending file: " + filename + " (" + String(file.size()) + " bytes)");
  
  server.sendHeader("Content-Disposition", "attachment; filename=" + filename.substring(1));
  server.streamFile(file, "audio/wav");
  file.close();
}

void handlePlay() {
  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "Missing file parameter");
    return;
  }
  
  String filename = server.arg("file");
  
  if (!filename.startsWith("/")) {
    filename = "/" + filename;
  }
  
  Serial.println("Play request: " + filename);
  
  if (!SPIFFS.exists(filename)) {
    Serial.println("❌ File not found: " + filename);
    server.send(404, "text/plain", "File not found");
    return;
  }
  
  File file = SPIFFS.open(filename, "r");
  if (!file) {
    server.send(500, "text/plain", "Failed to open file");
    return;
  }
  
  server.streamFile(file, "audio/wav");
  file.close();
}

void handleDelete() {
  if (!server.hasArg("file")) {
    server.sendHeader("Location", "/");
    server.send(303);
    return;
  }
  
  String filename = server.arg("file");
  
  if (!filename.startsWith("/")) {
    filename = "/" + filename;
  }
  
  if (SPIFFS.exists(filename)) {
    SPIFFS.remove(filename);
    Serial.println("🗑️ Deleted: " + filename);
  }
  
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleDeleteAll() {
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  int deleteCount = 0;
  
  while (file) {
    String filename = String(file.name());
    file.close();
    
    if (filename.endsWith(".wav")) {
      SPIFFS.remove(filename);
      deleteCount++;
    }
    
    file = root.openNextFile();
  }
  
  fileCounter = 0;
  Serial.printf("🗑️ Deleted %d files\n", deleteCount);
  
  server.sendHeader("Location", "/");
  server.send(303);
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/download", handleDownload);
  server.on("/play", handlePlay);
  server.on("/delete", handleDelete);
  server.on("/deleteall", handleDeleteAll);
  
  server.begin();
  Serial.println("✓ Web server started");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n╔════════════════════════════════════╗");
  Serial.println("║  ESP32 I2S Voice Recorder v2.0    ║");
  Serial.println("╚════════════════════════════════════╝\n");
  
  setupI2S();
  
  if (!SPIFFS.begin(true)) {
    Serial.println("❌ SPIFFS failed!");
    while (1) delay(1000);
  }
  
  Serial.println("✓ SPIFFS initialized");
  Serial.printf("📊 Storage: %d / %d bytes\n", SPIFFS.usedBytes(), SPIFFS.totalBytes());
  
  // นับไฟล์ที่มีอยู่
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    if (String(file.name()).endsWith(".wav")) {
      fileCounter++;
    }
    file = root.openNextFile();
  }
  Serial.printf("📁 Found %d existing recordings\n", fileCounter);
  
  Serial.printf("\n📡 Connecting to WiFi: %s", ssid);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi Connected!");
    Serial.print("🌐 IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.println("👉 เปิด IP นี้ในเบราว์เซอร์เพื่อดูไฟล์บันทึก\n");
    
    setupWebServer();
  } else {
    Serial.println("\n❌ WiFi failed! Running offline...\n");
  }
  
  Serial.println("🎤 Listening for wake word 'Jarvis'...");
  Serial.printf("📊 Current threshold: %d\n", WAKE_THRESHOLD);
  Serial.println("💡 ดูค่า Volume ที่แสดงเพื่อปรับ WAKE_THRESHOLD\n");
}

void loop() {
  server.handleClient();
  
  if (!isRecording) {
    if (detectWakeWord()) {
      isRecording = true;
      
      deleteOldestFile();
      
      char filename[32];
      sprintf(filename, "/rec_%03d.wav", fileCounter++);
      
      recordAudio(filename);
      
      isRecording = false;
      Serial.println("🎤 Listening again...\n");
    }
  }
  
  delay(10);
}