#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <FirebaseClient.h>
#include <time.h>
#include <WiFiManager.h> 

// ===== Konfigurasi WiFi dan Firebase =====
#define WIFI_SSID "password nya glenn2702"
#define WIFI_PASSWORD "glenn2702"
#define Web_API_KEY "AIzaSyALkKo0s2XQ45M7p0yMECzqatIolb2K2s0"
#define DATABASE_URL "https://healthbot-ceb8d-default-rtdb.asia-southeast1.firebasedatabase.app" 
#define USER_EMAIL "glennimmanuel8@gmail.com"
#define USER_PASS "lovepetra27"

// ===== Konfigurasi NTP untuk timestamp =====
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 25200      // GMT+7 timezone (dalam detik) - sesuaikan dengan lokasi Anda
#define DAYLIGHT_OFFSET_SEC 0     // Tidak menggunakan daylight saving

UserAuth user_auth(Web_API_KEY, USER_EMAIL, USER_PASS);
FirebaseApp app;
WiFiClientSecure ssl_client;
using AsyncClient = AsyncClientClass;
AsyncClient aClient(ssl_client);
RealtimeDatabase Database;
WiFiManager wifiManager;

// ===== Variabel ECG =====
int ecgValue = 0;
int threshold = 3000;        // Dinaikkan berdasarkan data yang diperoleh
unsigned long lastBeat = 0;
int beatCount = 0;
unsigned long lastBpmCheck = 0;
unsigned long lastBpmSend = 0;
int beats[10];               // Array untuk menyimpan interval detak terakhir
int beatIndex = 0;
bool peaked = false;         // Flag untuk deteksi puncak

// ===== Format timestamp =====
char timeStampString[30];

void setup() {
  Serial.begin(115200);
  pinMode(40, INPUT);
  pinMode(41, INPUT);
  wifiManager.autoConnect();

  // Koneksi Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(300);
  }
  Serial.println("\nWi-Fi connected!");

  // Konfigurasi waktu NTP
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  Serial.println("Waiting for NTP time sync...");
  
  // Tunggu hingga waktu tersinkronisasi
  time_t now = 0;
  while (now < 1000000000) {
    delay(500);
    time(&now);
  }
  Serial.println("NTP time synchronized!");

  // Setup SSL dan Firebase
  ssl_client.setInsecure();
  ssl_client.setConnectionTimeout(1000);
  ssl_client.setHandshakeTimeout(5);

  initializeApp(aClient, app, getAuth(user_auth), processData, "authTask");
  app.getApp<RealtimeDatabase>(Database);
  Database.url(DATABASE_URL);
}

// Fungsi untuk mendapatkan timestamp saat ini
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
    return "failed";
  }
  
  // Format: YYYY-MM-DD_HH:MM:SS
  strftime(timeStampString, sizeof(timeStampString), "%Y-%m-%d_%H:%M:%S", &timeinfo);
  return String(timeStampString);
}

void loop() {
  app.loop();  // Penting untuk FirebaseClient

  if (!app.ready()) {
    Serial.println("Firebase belum siap");
    delay(1000);
    return;
  }

  // Cek kondisi lead-off (elektroda terlepas)
  if (digitalRead(40) == 1 || digitalRead(41) == 1) {
    Serial.println("Lead off condition detected");
    delay(500);
    return;
  }

  // Baca nilai ECG
  ecgValue = analogRead(A0);
  Serial.print("ECG: ");
  Serial.println(ecgValue);

  // Algoritma deteksi detak yang lebih robust
  unsigned long now = millis();
  
  // Deteksi puncak naik
  if (ecgValue > threshold && !peaked && (now - lastBeat > 300)) { // Refractory period 300ms
    peaked = true;
    
    // Hitung interval detak
    if (lastBeat != 0) {
      unsigned long beatInterval = now - lastBeat;
      
      // Filter interval yang masuk akal (30-220 BPM)
      if (beatInterval > 273 && beatInterval < 2000) {
        beats[beatIndex] = beatInterval;
        beatIndex = (beatIndex + 1) % 10;
        beatCount++;
        
        // Perhitungan BPM secara real-time
        int bpm = 60000 / beatInterval;
        Serial.print("Current BPM: ");
        Serial.println(bpm);
      }
    }
    
    lastBeat = now;
  } 
  // Reset flag setelah sinyal turun kembali
  else if (ecgValue < (threshold - 200) && peaked) {
    peaked = false;
  }

  // Hitung BPM rata-rata setiap 3 detik dan kirim ke Firebase
  if (now - lastBpmCheck >= 3000 && beatCount >= 3) {
    lastBpmCheck = now;
    
    // Hitung rata-rata interval
    unsigned long totalInterval = 0;
    int validBeats = min(beatCount, 10);
    
    for (int i = 0; i < validBeats; i++) {
      totalInterval += beats[i];
    }
    
    int avgBpm = 60000 / (totalInterval / validBeats);
    
    // Kirim ke Firebase jika nilainya masuk akal (30-220 BPM)
    if (avgBpm >= 30 && avgBpm <= 220 && (now - lastBpmSend >= 10000)) {
      lastBpmSend = now;
      
      // Dapatkan timestamp saat ini
      String timestamp = getTimestamp();
      
      // Buat path untuk guest (data dari sensor asli)
      String guestBpmPath = "/bpm_history/guest/" + timestamp;
      
      Serial.print("Sending BPM to Firebase: ");
      Serial.print(avgBpm);
      Serial.print(" at ");
      Serial.println(timestamp);
      Serial.print("Path: ");
      Serial.println(guestBpmPath);
      
      // Simpan ke lokasi baru sesuai struktur database
      // Path: /bpm_history/guest/timestamp dengan value BPM
      Database.set<int>(aClient, guestBpmPath.c_str(), avgBpm, processData, "send_guest_bpm_history");
      
      // Optional: Jika masih ingin menyimpan nilai terbaru di root level
      Database.set<int>(aClient, "/bpm", avgBpm, processData, "send_current_bpm");
    }
  }
  
  delay(10); // Sedikit lebih lambat dari sebelumnya untuk stabilitas
}

// Callback hasil dari Firebase
void processData(AsyncResult &aResult) {
  if (!aResult.isResult()) return;

  if (aResult.isEvent())
    Firebase.printf("Event: %s | Msg: %s | Code: %d\n", aResult.uid().c_str(), aResult.eventLog().message().c_str(), aResult.eventLog().code());

  if (aResult.isDebug())
    Firebase.printf("Debug: %s | Msg: %s\n", aResult.uid().c_str(), aResult.debug().c_str());

  if (aResult.isError())
    Firebase.printf("Error: %s | Msg: %s | Code: %d\n", aResult.uid().c_str(), aResult.error().message().c_str(), aResult.error().code());

  if (aResult.available())
    Firebase.printf("Success [%s] | Payload: %s\n", aResult.uid().c_str(), aResult.c_str());
}