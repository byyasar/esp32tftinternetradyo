#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <vs1053_ext.h>
#include <XPT2046_Touchscreen.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <SD.h>

// Renk Paleti (Premium UI)
#define C_BG      0x0000
#define C_DARK    0x2124
#define C_BLUE    0x0217
#define C_ACCENT  0x07E0
#define C_TEXT    0xFFFF
#define C_GRAY    0x8410
#define C_WARN    0xFFE0
#define C_ERROR   0xF800
#define C_HEADER  0x10A2 
#define C_FOOTER  0x10A2 

// VS1053 Pinleri
#define VS1053_CS     10
#define VS1053_DCS     9
#define VS1053_DREQ    8
#define VS1053_RST    4 

// VS1053 SPI Pinleri
#define VS_MOSI   11
#define VS_MISO   13
#define VS_SCK    12

#define SD_CS   17
// SD kart VS1053 ile AYNI HSPI bus paylasiyor → SCK/MOSI/MISO pinleri aynı
// Donanim: SD kartın SCK=12, MOSI=11, MISO=13 pinlerine baglanmali!
#define SD_SCK  VS_SCK   // 12
#define SD_MOSI VS_MOSI  // 11
#define SD_MISO VS_MISO  // 13

// Touch SPI Pinleri (AYRI BUS!)
#define TOUCH_CS    1
#define TOUCH_CLK   42
#define TOUCH_DIN   2   // MOSI
#define TOUCH_DO    41  // MISO



// Touch icin ayri SPI bus
SPIClass touchSPI(FSPI); // SPI0/FSPI
XPT2046_Touchscreen ts(TOUCH_CS);

// SD Kart + VS1053 AYNI HSPI bus paylasiyor (ayni fiziksel pinler, farkli CS)
SPIClass sdSPI(HSPI); // VS1053 ile paylaşılan HSPI

// Cihaz Modu
enum PlayMode { MODE_RADIO, MODE_SD };
PlayMode currentMode = MODE_RADIO;

// SD MP3 listesi
#define MAX_MP3_FILES 50
String mp3Files[MAX_MP3_FILES];
int mp3Count = 0;
int currentMp3 = 0;
bool sdAvailable = false;
bool sdPlaying = false;

const char* ssid = "ASUS";
const char* password = "uTF52pvs";

// Istasyon Listesi (Sadece HTTP - Calisan Linkler)
const char *stations[] = {
    "http://kralpopwmp.radyotvonline.com/kralpopmp3",
    "http://officialbestfm.radyotvonline.net/bestfmofficial",
    "http://radyo.yayindakiler.com:4044/;;",
    "http://46.20.3.231/radyovivampeg",
    "http://ntvrdwmp.radyotvonline.com/ntvradyomp3",
    "http://stream.radyo45lik.com:4545/stream",
    "http://kralfmwmp.radyotvonline.com/kralfmmp3",
    "http://officialbestfm.radyotvonline.net/babaradyo",
    "http://radyo.yayindakiler.com:4112/;;",
    "http://radyo.yayin.com.tr:5894/;",
    "http://radyo.yayin.com.tr:4052/;",
    "http://radyo.yayindakiler.com:3000/;",
    "http://radyo.yayindakiler.com:4174/;",
    "http://yayin.turkhosted.com:6006/stream",
    "http://radyo.yayin.com.tr:4130/;",
    "http://46.20.3.229/",
    "http://live.bodrumfm.org/stream.mp3",
    "http://usa4.fastcast4u.com:5414",
    "http://95.173.162.184:7350/"
};

const char *sName[] = {
    "KRAL POP", "BEST FM", "SLOW TURK", "RADYO VIVA", "NTV RADYO",
    "RADYO 45LIK", "KRAL FM", "BABA RADYO", "RADYO CAN", "RADYO KULUP",
    "RADYO MEGA", "GENC KRAL", "RADYO EREGLI", "RADYO EKIN", "ISTANBUL FM",
    "SHOW RADYO", "BODRUM FM", "IZMIR FM", "DESIBEL 94"
};

const char *sGenre[] = {
    "Pop Muzik", "Pop Muzik", "Slow Muzik", "Pop Muzik", "Haber",
    "Nostalji", "Arabesk", "Arabesk", "Turku", "Pop Muzik",
    "Pop Muzik", "Pop Muzik", "Pop Muzik", "Turku", "Pop Muzik",
    "Pop Muzik", "Pop Muzik", "Pop Muzik", "Pop Muzik"
};

int currentStation = 0; 
int totalStations = sizeof(stations) / sizeof(stations[0]);
int currentVolume = 21; // vs1053 volume (0=max loud, 100=silent)

// vsSPI artık sdSPI ile aynı bus - ayrı tanıma gerek yok

String currentStreamTitle = "";
String currentStationName = "";
String weatherString = "Bekleniyor...";
String timeStr = "--:--";

TFT_eSPI tft = TFT_eSPI();
TFT_eSPI_Button btnPrev;
TFT_eSPI_Button btnVolDown;
TFT_eSPI_Button btnVolUp;
TFT_eSPI_Button btnNext;

VS1053 player(VS1053_CS, VS1053_DCS, VS1053_DREQ, HSPI, VS_MOSI, VS_MISO, VS_SCK);

// Alt klasörler dahil recursive MP3 tarama
void scanSdMp3Recursive(File dir, String basePath) {
    while (mp3Count < MAX_MP3_FILES) {
        File f = dir.openNextFile();
        if (!f) break;
        String name = String(f.name());
        if (f.isDirectory()) {
            // Alt klasöre gir
            String subPath = basePath + name + "/";
            Serial.printf("Klasor: %s\n", subPath.c_str());
            scanSdMp3Recursive(f, subPath);
        } else {
            String upperName = name;
            upperName.toUpperCase();
            if (upperName.endsWith(".MP3")) {
                String fullPath = basePath + name;
                mp3Files[mp3Count++] = fullPath;
                Serial.printf("MP3 bulundu: %s\n", fullPath.c_str());
            }
        }
        f.close();
    }
}

void scanSdMp3() {
    mp3Count = 0;
    if (!sdAvailable) return;
    File root = SD.open("/");
    if (!root || !root.isDirectory()) { root.close(); return; }
    scanSdMp3Recursive(root, "/");
    root.close();
    Serial.printf("SD: Toplam %d MP3 dosyasi bulundu\n", mp3Count);
}

String filterTurkce(String text) {
  text.replace("ı", "i"); text.replace("İ", "I");
  text.replace("ç", "c"); text.replace("Ç", "C");
  text.replace("ş", "s"); text.replace("Ş", "S");
  text.replace("ö", "o"); text.replace("Ö", "O");
  text.replace("ü", "u"); text.replace("Ü", "U");
  text.replace("ğ", "g"); text.replace("Ğ", "G");
  return text;
}

void printCentered(String text, int y, int size, uint16_t color) {
  tft.setTextSize(size);
  tft.setTextColor(color);
  int w = text.length() * 6 * size; 
  int x = (320 - w) / 2;
  if(x < 0) x = 0;
  tft.setCursor(x, y);
  tft.print(text);
}

void getWeather() {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("http://api.open-meteo.com/v1/forecast?latitude=37.3712&longitude=36.0964&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m&timezone=auto");
    int httpCode = http.GET();
    if (httpCode > 0) {
      String payload = http.getString();
      DynamicJsonDocument doc(1024);
      deserializeJson(doc, payload);
      float temp = doc["current"]["temperature_2m"];
      int hum = doc["current"]["relative_humidity_2m"];
      weatherString = "KADIRLI " + String(temp, 1) + "C %" + String(hum);
    }
    http.end();
  }
}

void updateTimeStrings() {
  struct tm t;
  if (getLocalTime(&t)) {
    char bTime[10]; sprintf(bTime, "%02d:%02d", t.tm_hour, t.tm_min);
    timeStr = String(bTime);
  }
}

void drawHeader() {
  tft.fillRect(0, 0, 320, 22, C_HEADER); 
  tft.drawFastHLine(0, 21, 320, C_BLUE);
  tft.setTextSize(1);
  tft.setTextColor(C_TEXT);
  tft.setCursor(5, 7);
  tft.print(weatherString); 

  int rssi = WiFi.RSSI();
  uint16_t sigColor = (rssi > -65) ? C_ACCENT : ((rssi > -80) ? C_WARN : C_ERROR);
  tft.fillRect(300, 12, 3, 3, sigColor);  
  tft.fillRect(305, 9, 3, 6, sigColor);  
  if(rssi > -75) tft.fillRect(310, 6, 3, 9, sigColor); 

  tft.setTextColor(C_GRAY);
  tft.setCursor(270, 7);
  tft.print(timeStr);
}

void drawFooter() {
  tft.fillRect(0, 185, 320, 15, C_FOOTER);
  tft.drawFastHLine(0, 184, 320, C_BLUE);
  tft.setTextSize(1);
  tft.setTextColor(C_GRAY);
  tft.setCursor(5, 189);
  tft.print("NET:Bagli");

  // Ortada ses seviyesi
  int volPct = map(currentVolume, 0, 21, 0, 100);
  tft.setTextColor(C_WARN);
  tft.setCursor(130, 189);
  tft.printf("SES:%d%%", volPct);

  String catName = filterTurkce(String(sGenre[currentStation]));
  int w = catName.length() * 6 + 12;
  tft.setCursor(315 - w, 189);
  tft.setTextColor(C_ACCENT);
  tft.print("[" + catName + "]");
}

void drawStationInfo() {
  if (currentMode == MODE_RADIO) {
    tft.fillRect(0, 22, 320, 120, C_BG);
    String sNameStr = String(sName[currentStation]);
    printCentered(sNameStr, 40, 3, C_TEXT);
    printCentered("KANAL " + String(currentStation + 1) + " / " + String(totalStations), 75, 1, C_ACCENT);
    if(currentStreamTitle.length() > 0) {
        printCentered(currentStreamTitle.substring(0, 35), 95, 1, TFT_CYAN);
    } else if (currentStationName.length() > 0) {
        printCentered(currentStationName.substring(0, 35), 95, 1, TFT_CYAN);
    }
  } else {
    // SD Modu
    tft.fillRect(0, 22, 320, 120, C_BG);
    printCentered("SD MUZIK", 35, 2, C_ACCENT);
    if (mp3Count == 0) {
        printCentered(sdAvailable ? "MP3 Bulunamadi" : "SD Kart Yok", 65, 1, C_ERROR);
    } else {
        String fname = mp3Files[currentMp3];
        if (fname.length() > 30) fname = fname.substring(0, 27) + "...";
        printCentered(fname, 65, 1, TFT_CYAN);
        printCentered(String(currentMp3 + 1) + " / " + String(mp3Count), 85, 1, C_GRAY);
        if (sdPlaying)
            printCentered(">>> CALIYOR <<<", 100, 1, C_ACCENT);
        else
            printCentered("|| DURDURULDU", 100, 1, C_WARN);
    }
  }
}

void drawVuMeter() {
  int yBase = 175;
  int xStart = 50; 
  int barWidth = 8;
  int barGap = 4;
  int numBars = 18;

  static int vuLevel = 0;
  int tgt = 0;
  bool isPlaying = player.isRunning(); 
  if (isPlaying) tgt = random(2, 16); 
  else tgt = 1;
  vuLevel = (vuLevel + tgt) / 2; 

  tft.fillRect(xStart, yBase - 16, 220, 16, C_BG);

  for (int i = 0; i < numBars; i++) {
    float env = 1.0 - abs(i - (numBars/2.0)) / (numBars/2.0); 
    int h = max(1, (int)(vuLevel * env + random(-1, 2)));
    if(!isPlaying) h = 1; 
    h = constrain(h, 1, 16);
    int x = xStart + i * (barWidth + barGap);
    uint16_t color = C_BLUE;
    if(h > 8) color = C_ACCENT;
    if(h > 12) color = C_WARN;
    tft.fillRect(x, yBase - h, barWidth, h, color);
  }
}

void drawUIButtons() {
    if (currentMode == MODE_RADIO) {
        btnPrev.initButton(&tft, 45, 220, 80, 30, C_HEADER, C_DARK, C_TEXT, (char*)"<< ", 1);
        btnVolDown.initButton(&tft, 130, 220, 60, 30, C_HEADER, C_DARK, C_WARN, (char*)"VOL-", 1);
        btnVolUp.initButton(&tft, 190, 220, 60, 30, C_HEADER, C_DARK, C_ACCENT, (char*)"VOL+", 1);
        btnNext.initButton(&tft, 275, 220, 80, 30, C_HEADER, C_DARK, C_TEXT, (char*)" >>", 1);
    } else {
        btnPrev.initButton(&tft, 45, 220, 80, 30, C_HEADER, C_DARK, C_TEXT, (char*)"<< ", 1);
        btnVolDown.initButton(&tft, 130, 220, 60, 30, C_HEADER, C_DARK, C_WARN, (char*)"PLAY", 1);
        btnVolUp.initButton(&tft, 190, 220, 60, 30, C_HEADER, C_DARK, C_ACCENT, (char*)"DUR", 1);
        btnNext.initButton(&tft, 275, 220, 80, 30, C_HEADER, C_DARK, C_TEXT, (char*)" >>", 1);
    }
    btnPrev.drawButton();
    btnVolDown.drawButton();
    btnVolUp.drawButton();
    btnNext.drawButton();
}

void switchToSD() {
    player.stop_mp3client();
    currentMode = MODE_SD;
    sdPlaying = false;
    tft.fillScreen(C_BG);
    drawHeader();
    drawStationInfo();
    drawFooter();
    drawUIButtons();
}

void switchToRadio() {
    if (sdPlaying) { player.stop_mp3client(); sdPlaying = false; }
    currentMode = MODE_RADIO;
    currentStreamTitle = "";
    currentStationName = "";
    tft.fillScreen(C_BG);
    drawHeader();
    drawStationInfo();
    drawFooter();
    drawUIButtons();
    printCentered("Baglaniyor...", 110, 1, C_WARN);
    player.connecttohost(stations[currentStation]);
}

void playSdFile(int idx) {
    if (!sdAvailable || mp3Count == 0) return;
    if (sdPlaying) {
        player.stop_mp3client();
        delay(300); // VS1053'ün önceki dosyayı bırakması için bekle
    }
    // mp3Files[] zaten tam yolu içeriyor (ör: /muzik/sarki.mp3)
    String path = mp3Files[idx];
    if (!path.startsWith("/")) path = "/" + path;
    Serial.printf("SD Oynat: %s\n", path.c_str());
    player.connecttoFS(SD, path.c_str());
    sdPlaying = true;
    drawStationInfo();
}

void drawBoot() {
  tft.fillScreen(C_BG);
  tft.fillRoundRect(140, 50, 40, 24, 4, C_DARK);
  tft.drawRoundRect(140, 50, 40, 24, 4, C_BLUE);
  tft.fillCircle(153, 62, 7, C_BG); 
  tft.drawCircle(168, 66, 3, C_BG);
  tft.drawCircle(190, 60, 10, C_BLUE);
  tft.drawCircle(195, 57, 15, C_ACCENT);
  printCentered("YSR", 100, 2, C_TEXT);
  printCentered("INTERNET", 130, 2, C_ACCENT);
  printCentered("RADYOSU", 160, 2, C_TEXT);
}

void drawBootMsg(String msg) {
  tft.fillRect(0, 200, 320, 15, C_BG);
  printCentered(msg, 202, 1, C_GRAY);
}

void setup() {
    Serial.begin(115200);

    tft.init();
    tft.setRotation(1);
    
    drawBoot();
    drawBootMsg("Sistem Baslatiliyor...");
    
    // Touch SPI bus'ini kendi pinleriyle baslat
    touchSPI.begin(TOUCH_CLK, TOUCH_DO, TOUCH_DIN, TOUCH_CS);
    ts.begin(touchSPI);
    ts.setRotation(1);
    Serial.println("Touch baslatildi: CS=1 CLK=42 DIN=2 DO=41");

    // SD init VS1053'den once yapilmayacak - VS1053 HSPI'yi hazirlasin once

    WiFi.begin(ssid, password);
    drawBootMsg("WiFi Baglaniyor...");
    int wifiWait = 0;
    while (WiFi.status() != WL_CONNECTED && wifiWait < 20) { 
        delay(500); 
        wifiWait++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        drawBootMsg("Saat Esitleniyor...");
        configTime(3 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        delay(1000); 

        drawBootMsg("Hava Durumu Aliniyor...");
        getWeather();
        updateTimeStrings();
    } else {
        drawBootMsg("WiFi Baglanamiyor!");
        delay(1000);
    }
    
    delay(500);

    tft.fillScreen(C_BG);
    drawHeader();
    drawStationInfo();
    drawFooter();
    drawUIButtons();

    // VS1053 Reset
    pinMode(VS1053_RST, OUTPUT);
    digitalWrite(VS1053_RST, LOW);
    delay(10);
    digitalWrite(VS1053_RST, HIGH);
    delay(100);

    player.begin();
    player.setVolume(21);

    // VS1053 HSPI'yi basalttiktan sonra SD karti ayni bus'a bagla
    // sdSPI.begin() cagirmiyoruz - VS1053 zaten HSPI'yi baslatmis durumda
    // SD_CS=17 farkli oldugu icin cs-based paylaşim calisiyor
    drawBootMsg("SD Kart Kontrol Ediliyor...");
    sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    delay(100);
    if (SD.begin(SD_CS, sdSPI, 4000000)) {
        sdAvailable = true;
        Serial.println("SD Kart OK!");
        scanSdMp3();
        Serial.printf("SD: %d MP3 bulundu\n", mp3Count);
    } else {
        sdAvailable = false;
        Serial.println("SD Kart Bulunamadi! (player.begin sonrasi)");
    }
    delay(300);
    drawStationInfo(); // SD durumunu ekranda guncelle

    player.connecttohost(stations[currentStation]);
}

// Debounce icin
unsigned long lastTouchTime = 0;
const unsigned long TOUCH_DEBOUNCE = 500; // 500ms

void loop() {
    player.loop();

    // Dokunmatik okuma (ayri SPI bus uzerinden)
    if (ts.touched() && (millis() - lastTouchTime > TOUCH_DEBOUNCE)) {
        TS_Point p = ts.getPoint();
        
        uint16_t tx = map(p.x, 300, 3800, 320, 0); 
        uint16_t ty = map(p.y, 300, 3800, 240, 0);
        
        Serial.printf("Touch: raw(%d,%d) -> map(%d,%d) mode=%d\n", p.x, p.y, tx, ty, currentMode);
        lastTouchTime = millis();

        // Ust alan (header) uzun dokunus = MOD DEGISTIR
        if (ty < 22) {
            if (currentMode == MODE_RADIO) {
                switchToSD();
            } else {
                switchToRadio();
            }
            return;
        }

        if (currentMode == MODE_RADIO) {
            // ---- RADYO MODU ----
            if (tx < 80) {
                // << PREV
                btnPrev.drawButton(true);
                currentStation--;
                if (currentStation < 0) currentStation = totalStations - 1;
                currentStreamTitle = ""; 
                currentStationName = "";
                drawStationInfo();
                drawFooter();
                printCentered("Baglaniyor...", 110, 1, C_WARN);
                player.connecttohost(stations[currentStation]);
                delay(200);
                btnPrev.drawButton();
            }
            else if (tx >= 80 && tx < 160) {
                // VOL-
                btnVolDown.drawButton(true);
                currentVolume -= 1;
                if (currentVolume < 0) currentVolume = 0;
                player.setVolume(currentVolume);
                int pct = map(currentVolume, 0, 21, 0, 100);
                tft.fillRect(100, 108, 120, 12, C_BG);
                printCentered("SES: " + String(pct) + "%", 110, 1, C_WARN);
                drawFooter();
                delay(150);
                btnVolDown.drawButton();
            }
            else if (tx >= 160 && tx < 240) {
                // VOL+
                btnVolUp.drawButton(true);
                currentVolume += 1;
                if (currentVolume > 21) currentVolume = 21;
                player.setVolume(currentVolume);
                int pct = map(currentVolume, 0, 21, 0, 100);
                tft.fillRect(100, 108, 120, 12, C_BG);
                printCentered("SES: " + String(pct) + "%", 110, 1, C_ACCENT);
                drawFooter();
                delay(150);
                btnVolUp.drawButton();
            }
            else {
                // >> NEXT
                btnNext.drawButton(true);
                currentStation++;
                if (currentStation >= totalStations) currentStation = 0;
                currentStreamTitle = "";
                currentStationName = "";
                drawStationInfo();
                drawFooter();
                printCentered("Baglaniyor...", 110, 1, C_WARN);
                player.connecttohost(stations[currentStation]);
                delay(200);
                btnNext.drawButton();
            }
        } else {
            // ---- SD MUZIK MODU ----
            if (tx < 80) {
                // << Onceki dosya
                btnPrev.drawButton(true);
                if (mp3Count > 0) {
                    currentMp3--;
                    if (currentMp3 < 0) currentMp3 = mp3Count - 1;
                    playSdFile(currentMp3);
                }
                delay(200);
                btnPrev.drawButton();
            }
            else if (tx >= 80 && tx < 160) {
                // PLAY
                btnVolDown.drawButton(true);
                if (mp3Count > 0 && !sdPlaying) {
                    playSdFile(currentMp3);
                }
                delay(150);
                btnVolDown.drawButton();
            }
            else if (tx >= 160 && tx < 240) {
                // DUR
                btnVolUp.drawButton(true);
                if (sdPlaying) {
                    player.stop_mp3client();
                    sdPlaying = false;
                    drawStationInfo();
                }
                delay(150);
                btnVolUp.drawButton();
            }
            else {
                // >> Sonraki dosya
                btnNext.drawButton(true);
                if (mp3Count > 0) {
                    currentMp3++;
                    if (currentMp3 >= mp3Count) currentMp3 = 0;
                    playSdFile(currentMp3);
                }
                delay(200);
                btnNext.drawButton();
            }
        }
    }

    // Radyo: Stream bilgisi guncelleme
    if (currentMode == MODE_RADIO) {
        static unsigned long lastInfo = 0;
        if (millis() - lastInfo > 3000) {
            lastInfo = millis();
            tft.fillRect(0, 105, 320, 15, C_BG);
            if(currentStreamTitle.length() > 0) {
                printCentered(currentStreamTitle.substring(0, 35), 110, 1, TFT_CYAN);
            } else if (player.isRunning()) {
                printCentered("Caliyor", 110, 1, C_ACCENT);
            }
        }
    }

    // SD: Dosya bitince sonrakini cal
    if (currentMode == MODE_SD && sdPlaying && !player.isRunning()) {
        currentMp3++;
        if (currentMp3 >= mp3Count) currentMp3 = 0;
        delay(500);
        playSdFile(currentMp3);
    }

    // Saat guncelleme
    static unsigned long lastTimeHeader = 0;
    if (millis() - lastTimeHeader > 1000) {
        lastTimeHeader = millis();
        updateTimeStrings();
        drawHeader();
        // SD modunda mod etiketi goster
        if (currentMode == MODE_SD) {
            tft.setTextColor(C_ACCENT);
            tft.setTextSize(1);
            tft.setCursor(130, 7);
            tft.print("[SD MUZIK]");
        }
    }

    // Hava durumu guncelleme (saatte bir)
    static unsigned long lastWeatherUpdate = 0;
    if (millis() - lastWeatherUpdate > 3600000) { 
        lastWeatherUpdate = millis();
        getWeather();
    }

    // VU metre guncelleme
    static unsigned long lastVU = 0;
    if (millis() - lastVU > 150) {
        lastVU = millis();
        drawVuMeter();
    }
}

void vs1053_showstreamtitle(const char *info) {
    currentStreamTitle = String(info);
}

void vs1053_showstation(const char *info) {
    currentStationName = String(info);
}
