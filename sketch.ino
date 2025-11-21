/*
╔══════════════════════════════════════════════════════════════════════╗
║                   🦊 COMPASS ASSISTANT PRO 🦊                        ║
║                                                                      ║
║    Sistema IoT Completo com Feedback Visual e Sonoro                ║
║    Plataforma: ESP32 | Display: OLED SSD1306 | LED RGB + Buzzer    ║
║                                                                      ║
║    Global Solution 2025 - FIAP                                      ║
║    Desenvolvido por: Julia Azevedo | Luís Barreto | Victor Hugo   ║
╚══════════════════════════════════════════════════════════════════════╝

FUNCIONALIDADES:
- Sincronização com Firebase Firestore (HTTP/REST)
- Alertas multissensoriais: LED RGB + Buzzer + Display OLED
- Escalação de urgência: Verde → Ciano → Amarelo → Laranja → Vermelho
- Controles físicos: 3 botões (Confirm, Snooze, Dismiss)
- Atualização automática a cada 2 minutos

FLUXO PRINCIPAL:
1. Conecta ao WiFi
2. Sincroniza horário (NTP)
3. Busca entrevistas do Firebase
4. Ordena por proximidade
5. Exibe alerta progressivo conforme se aproxima
6. Aguarda interação do usuário (botões)
7. Retorna ao passo 3 (a cada 2 minutos)
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFiClientSecure.h>

// ══════════════════════════════════════════════════════════════════════════════
// 🔧 CONFIGURAÇÃO: CREDENCIAIS E PINOS
// ══════════════════════════════════════════════════════════════════════════════
const char* FIREBASE_URL = "https://firestore.googleapis.com/v1/projects/compass-d89ed/databases/(default)/documents/interviews?key=AIzaSyBHOOLHgbOm8GcAf2_Diqw84XCgVECcdzo";
const char* FIREBASE_BASE = "https://firestore.googleapis.com/v1/projects/compass-d89ed/databases/(default)/documents/interviews/";
const char* FIREBASE_KEY = "?updateMask.fieldPaths=status&key=AIzaSyBHOOLHgbOm8GcAf2_Diqw84XCgVECcdzo";
const char* ssid = "Wokwi-GUEST";
const char* password = "";

// Pinos
#define BTN_SNOOZE 32
#define BTN_DISMISS 33
#define BTN_CONFIRM 14
#define LED_R 25      // Pino RED do LED RGB
#define LED_G 26      // Pino GREEN do LED RGB
#define LED_B 27      // Pino BLUE do LED RGB
#define BUZZER 15
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiClientSecure client;
HTTPClient http;

// ══════════════════════════════════════════════════════════════════════════════
// 📊 ESTRUTURA DE DADOS: ENTREVISTAS E ESTADO
// ══════════════════════════════════════════════════════════════════════════════
// Armazena informações de cada entrevista sincronizada com Firebase
struct Interview {
  String candidateName;
  String position;
  String type;
  time_t dateTime;
  int duration;
  bool dismissed;
  bool snoozed;
  unsigned long snoozeUntil;
  String documentId;
};

Interview interviews[10];
int interviewCount = 0;
Interview* nextInterview = nullptr;

// ══════════════════════════════════════════════════════════════════════════════
// ⏱️ CONTROLE: TIMERS E DEBOUNCING
// ══════════════════════════════════════════════════════════════════════════════
// Gerencia intervalos de atualização, debounce de botões e alternância de telas
unsigned long lastCheck = 0;
const unsigned long CHECK_INTERVAL = 120000;
unsigned long lastDebounce[3] = {0, 0, 0};
const unsigned long debounceDelay = 300;
unsigned long lastDisplayUpdate = 0;
int currentScreen = 0; // 0=main, 1=details
unsigned long lastScreenChange = 0;
bool ledBlink = false;
unsigned long lastBlink = 0;
uint8_t currentLedR = 0, currentLedG = 0, currentLedB = 0;

// ══════════════════════════════════════════════════════════════════════════════
// 💡 LED RGB: CONTROLE E STATUS VISUAL
// ══════════════════════════════════════════════════════════════════════════════
// Cores indicam urgência:
//  🟢 Verde   = Tranquilo (sem entrevistas próximas)
//  🔵 Ciano   = Programado (> 1 hora)
//  🟡 Amarelo = Atenção (< 1 hora)
//  🟠 Laranja = Alerta (< 15 min)
//  🔴 Vermelho= Urgente (< 5 min ou AGORA)
//  ⚡ Piscando= Em andamento (agora!)
void setLEDRGB(uint8_t r, uint8_t g, uint8_t b) {
  // Só atualiza se mudou (evita spam no Serial)
  if (r == currentLedR && g == currentLedG && b == currentLedB) {
    return;
  }
  
  currentLedR = r;
  currentLedG = g;
  currentLedB = b;
  
  // Controle direto - ESP32 -> Resistor -> LED (cátodo comum)
  // HIGH = acende o LED
  digitalWrite(LED_R, r > 0 ? HIGH : LOW);
  digitalWrite(LED_G, g > 0 ? HIGH : LOW);
  digitalWrite(LED_B, b > 0 ? HIGH : LOW);
  
  // Debug apenas quando muda
  Serial.printf("🎨 LED RGB: R=%s G=%s B=%s = ", 
    r > 0 ? "ON" : "OFF",
    g > 0 ? "ON" : "OFF", 
    b > 0 ? "ON" : "OFF");
    
  if (r > 200 && g < 50 && b < 50) Serial.println("🔴 Vermelho");
  else if (r > 200 && g > 200 && b < 50) Serial.println("🟡 Amarelo");
  else if (r > 200 && g > 100 && b < 50) Serial.println("🟠 Laranja");
  else if (r < 50 && g > 200 && b < 50) Serial.println("🟢 Verde");
  else if (r < 50 && g > 200 && b > 200) Serial.println("🔵 Ciano");
  else if (r < 50 && g < 50 && b > 200) Serial.println("🔵 Azul");
  else if (r == 0 && g == 0 && b == 0) Serial.println("⚫ Desligado");
  else Serial.printf("RGB(%d,%d,%d)\n", r, g, b);
}

void updateLEDStatus() {
  // 📌 DESCRIÇÃO: Atualiza LED RGB baseado na urgencia da proxima entrevista
  // 🎨 ESQUEMA DE CORES: Verde (calmo) → Ciano (longe) → Amarelo (1h) → Laranja (15m) → Vermelho (5m) → Piscante (AGORA!)
  // ⏰ CÁLCULO: Diferença de tempo (agora vs. dateTime da entrevista)
  // 🔴 ESPECIAL: Piscante 500ms quando diff < 0 (entrevista em andamento/atrasada)

  if (!nextInterview) {
    // Verde = Tranquilo
    setLEDRGB(0, 255, 0);
    return;
  }
  
  time_t now;
  time(&now);
  long diff = difftime(nextInterview->dateTime, now);
  
  if (diff < 0) {
    // Vermelho PISCANDO = AGORA!
    if (millis() - lastBlink > 500) {
      ledBlink = !ledBlink;
      lastBlink = millis();
    }
    setLEDRGB(ledBlink ? 255 : 0, 0, 0);
  } else if (diff < 300) {
    // Vermelho = < 5min
    setLEDRGB(255, 0, 0);
  } else if (diff < 900) {
    // Laranja = < 15min
    setLEDRGB(255, 128, 0);
  } else if (diff < 3600) {
    // Amarelo = < 1h
    setLEDRGB(255, 255, 0);
  } else {
    // Ciano = Distante
    setLEDRGB(0, 255, 255);
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// 🔊 BUZZER: ALERTAS SONOROS
// ══════════════════════════════════════════════════════════════════════════════
// Sons progressivos indicam urgência (suave → moderado → urgente)
void playTone(int freq, int duration) {
  tone(BUZZER, freq, duration);
  delay(duration);
  noTone(BUZZER);
}

void alertSound(int urgency) {
  // 0=suave, 1=moderado, 2=urgente
  if (urgency == 0) {
    playTone(800, 100);
    delay(50);
    playTone(1000, 100);
  } else if (urgency == 1) {
    for (int i = 0; i < 2; i++) {
      playTone(1200, 150);
      delay(100);
    }
  } else {
    for (int i = 0; i < 3; i++) {
      playTone(1500, 200);
      delay(100);
    }
  }
}

void successSound() {
  playTone(1000, 100);
  delay(50);
  playTone(1200, 100);
  delay(50);
  playTone(1500, 150);
}

void errorSound() {
  playTone(400, 300);
  delay(100);
  playTone(300, 300);
}

// ══════════════════════════════════════════════════════════════════════════════
// 🛠️ FIREBASE UPDATE
// ══════════════════════════════════════════════════════════════════════════════
// Atualiza o status de uma entrevista específica no Firebase
bool updateInterviewStatus(const char* documentId, const char* newStatus) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi desconectado - Impossivel atualizar status");
    return false;
  }
  
  // Monta a URL do documento específico
  String updateUrl = String(FIREBASE_BASE) + documentId + FIREBASE_KEY;
  
  // JSON do body com o novo status
  StaticJsonDocument<256> doc;
  doc["fields"]["status"]["stringValue"] = newStatus;
  
  String jsonBody;
  serializeJson(doc, jsonBody);
  
  Serial.println("\n🔄 Atualizando status no Firebase...");
  Serial.printf("📝 Documento: %s\n", documentId);
  Serial.printf("✏️  Novo status: %s\n", newStatus);
  Serial.printf("🌐 URL: %s\n", updateUrl.c_str());
  
  HTTPClient httpUpdate;
  httpUpdate.begin(client, updateUrl);
  httpUpdate.addHeader("Content-Type", "application/json");
  
  int httpCode = httpUpdate.PATCH(jsonBody);
  
  if (httpCode == 200) {
    Serial.println("✅ Status atualizado com sucesso no Firebase!");
    httpUpdate.end();
    return true;
  } else {
    Serial.printf("❌ Erro ao atualizar: HTTP %d\n", httpCode);
    if (httpCode > 0) {
      String response = httpUpdate.getString();
      Serial.println("Resposta: " + response);
    }
    httpUpdate.end();
    return false;
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// 🛠️ FUNÇÕES AUXILIARES: PARSING E FORMATAÇÃO
// ══════════════════════════════════════════════════════════════════════════════
// Converte timestamps Firebase, formata datas, ordena entrevistas
time_t parseFirebaseTimestamp(const char* timestamp) {
  struct tm timeinfo = {0};
  int year, month, day, hour, minute, second;
  
  if (sscanf(timestamp, "%d-%d-%dT%d:%d:%d",
             &year, &month, &day, &hour, &minute, &second) == 6) {
    
    timeinfo.tm_year = year - 1900;
    timeinfo.tm_mon = month - 1;
    timeinfo.tm_mday = day;
    timeinfo.tm_hour = hour;
    timeinfo.tm_min = minute;
    timeinfo.tm_sec = second;
    timeinfo.tm_isdst = 0;
    
    time_t utc_time = mktime(&timeinfo);
    time_t local_time = utc_time - 10800;
    
    return local_time;
  }
  return 0;
}

String formatTimeUntil(time_t interviewTime) {
  time_t now;
  time(&now);
  long diff = difftime(interviewTime, now);
  
  if (diff < -300) return "PASSOU";
  if (diff < 0) return "AGORA!";
  if (diff < 60) return String(diff) + "s";
  if (diff < 3600) return String(diff / 60) + "min";
  if (diff < 86400) {
    int hours = diff / 3600;
    int mins = (diff % 3600) / 60;
    return String(hours) + "h" + String(mins) + "m";
  }
  return String(diff / 86400) + "d";
}

String formatDateTime(time_t t) {
  struct tm* timeinfo = localtime(&t);
  char buffer[20];
  sprintf(buffer, "%02d/%02d %02d:%02d",
          timeinfo->tm_mday, timeinfo->tm_mon + 1,
          timeinfo->tm_hour, timeinfo->tm_min);
  return String(buffer);
}

void sortInterviewsByProximity() {
  // 📌 DESCRIÇÃO: Ordena entrevistas por data/hora e seleciona proxima ativa
  // 🔍 LÓGICA: Bubble sort crescente + filtra dismissed/snoozed + seleciona primeira valida
  // 🎯 RESULTADO: `nextInterview` apontando para proxima urgente (ou nullptr se nenhuma)
  // ⏰ CASO DE BORDA: Respeita snooze (ignora por 5 min) e dismissals
  for (int i = 0; i < interviewCount - 1; i++) {
    for (int j = 0; j < interviewCount - i - 1; j++) {
      if (interviews[j].dateTime > interviews[j + 1].dateTime) {
        Interview temp = interviews[j];
        interviews[j] = interviews[j + 1];
        interviews[j + 1] = temp;
      }
    }
  }
  
  nextInterview = nullptr;
  time_t now;
  time(&now);
  
  for (int i = 0; i < interviewCount; i++) {
    if (interviews[i].dismissed) continue;
    
    if (interviews[i].snoozed && millis() < interviews[i].snoozeUntil) {
      continue;
    }
    
    long diff = difftime(interviews[i].dateTime, now);
    if (diff < -300) continue;
    
    nextInterview = &interviews[i];
    break;
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// 📺 DISPLAY: TELAS MÚLTIPLAS (BOOT, MAIN, DETALHES, FEEDBACKS)
// ══════════════════════════════════════════════════════════════════════════════
// Exibe informações do candidato com alternância automática entre resumo e detalhes
void drawCenteredText(String text, int y, int size = 1) {
  display.setTextSize(size);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((128 - w) / 2, y);
  display.print(text);
}

void showBoot() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // Logo simples COMPASS
  display.setTextSize(3);
  drawCenteredText("COMPASS", 5, 3);
  
  display.setTextSize(1);
  drawCenteredText("Assistant Pro", 38);
  drawCenteredText("IoT RGB", 50);
  
  display.display();
  delay(2000);
}

void showStatus(String msg) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  drawCenteredText(msg, 28);
  display.display();
}

void showMainScreen() {
  // 📌 DESCRIÇÃO: Tela principal com resumo da proxima entrevista
  // 📺 CONTEÚDO: Cargo + Tempo em minutos + Botoes de acao disponíveis
  // 🎯 CASOS: Sem alerta (^_^) | Com alerta urgente (mostra timing)
  // ⌨️  INTERAÇÃO: Dismiss (btn1) ou Snooze 5min (btn2) disponíveis
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  if (!nextInterview) {
    // ========== TELA: TUDO TRANQUILO ==========
    display.setTextSize(3);
    drawCenteredText("^_^", 10, 3);
    
    display.setTextSize(1);
    drawCenteredText("TUDO CERTO!", 42);
    drawCenteredText("Sem alertas", 54);
    
    display.display();
    return;
  }
  
  // ========== TELA PRINCIPAL: INFO RESUMIDA ==========
  time_t now;
  time(&now);
  long diff = difftime(nextInterview->dateTime, now);
  
  // Status no topo
  display.setTextSize(2);
  if (diff < 0) {
    drawCenteredText("AGORA!", 2, 2);
  } else if (diff < 300) {
    drawCenteredText("URGENTE", 2, 2);
  } else if (diff < 900) {
    drawCenteredText("ALERTA", 2, 2);
  } else {
    drawCenteredText("PROXIMO", 2, 2);
  }
  
  display.drawFastHLine(0, 20, 128, SSD1306_WHITE);
  
  // Nome do candidato (GRANDE)
  display.setTextSize(1);
  String name = nextInterview->candidateName;
  if (name.length() > 21) name = name.substring(0, 20) + ".";
  drawCenteredText(name, 26);
  
  // Tempo restante (MUITO GRANDE)
  display.setTextSize(3);
  String timeStr = formatTimeUntil(nextInterview->dateTime);
  drawCenteredText(timeStr, 38, 3);
  
  display.display();
}

void showDetailsScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  if (!nextInterview) {
    showMainScreen();
    return;
  }
  
  // ========== TELA DETALHES: CARGO + TIPO + HORA ==========
  display.setTextSize(1);
  drawCenteredText("DETALHES", 2);
  
  display.drawFastHLine(0, 12, 128, SSD1306_WHITE);
  
  // Cargo (quebrado em 2 linhas se necessário)
  display.setCursor(0, 18);
  display.print("Cargo:");
  
  String pos = nextInterview->position;
  display.setTextSize(1);
  
  if (pos.length() <= 21) {
    // Cabe em 1 linha
    display.setCursor(0, 28);
    display.print(pos);
  } else {
    // Quebra em 2 linhas
    String line1 = pos.substring(0, 21);
    String line2 = pos.substring(21);
    if (line2.length() > 21) line2 = line2.substring(0, 20) + ".";
    
    display.setCursor(0, 28);
    display.print(line1);
    display.setCursor(0, 38);
    display.print(line2);
  }
  
  // Tipo e horário
  display.drawFastHLine(0, 50, 128, SSD1306_WHITE);
  
  String typeStr = (nextInterview->type == "video") ? "VIDEO" : "PRESENCIAL";
  display.setCursor(2, 54);
  display.print(typeStr);
  
  String dateStr = formatDateTime(nextInterview->dateTime);
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(dateStr, 0, 0, &x1, &y1, &w, &h);
  display.setCursor(128 - w - 2, 54);
  display.print(dateStr);
  
  display.display();
}

void showNextInterview() {
  // Alterna entre telas a cada 5 segundos
  if (millis() - lastScreenChange > 5000) {
    currentScreen = 1 - currentScreen;
    lastScreenChange = millis();
  }
  
  if (currentScreen == 0) {
    showMainScreen();
  } else {
    showDetailsScreen();
  }
}

void showDismissed() {
  display.clearDisplay();
  
  display.setTextSize(3);
  drawCenteredText("OK!", 8, 3);
  
  display.setTextSize(1);
  drawCenteredText("DISPENSADO", 38);
  drawCenteredText("Alerta removido", 50);
  
  display.display();
}

void showSnoozed() {
  display.clearDisplay();
  
  display.setTextSize(2);
  drawCenteredText("ZzZ", 10, 2);
  
  display.setTextSize(1);
  drawCenteredText("SONECA ATIVADA", 34);
  drawCenteredText("Lembro em 5 min", 46);
  
  display.display();
}

void showConfirmed() {
  display.clearDisplay();
  
  display.setTextSize(2);
  drawCenteredText("CHECK", 8, 2);
  
  display.setTextSize(1);
  drawCenteredText("CONFIRMADO!", 34);
  drawCenteredText("Presente na sala", 46);
  
  display.display();
}

void showError() {
  display.clearDisplay();
  
  display.setTextSize(2);
  drawCenteredText("ERRO!", 8, 2);
  
  display.setTextSize(1);
  drawCenteredText("Falha ao atualizar", 34);
  drawCenteredText("Tente novamente", 46);
  
  display.display();
}

// ══════════════════════════════════════════════════════════════════════════════
// 🌐 NETWORK: WIFI E FIREBASE
// ══════════════════════════════════════════════════════════════════════════════
// Conecta ao WiFi e sincroniza entrevistas via REST API
void setupWiFi() {
    WiFi.begin("Wokwi-GUEST", "", 6);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 200) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
}

void fetchInterviews() {
  // 📌 DESCRIÇÃO: Sincroniza entrevistas via GET request na Firebase Firestore
  // 📥 PARÂMETROS: Nenhum (usa FIREBASE_URL global)
  // 📤 RETORNO: Popula array `interviews[10]` e atualiza `interviewCount`
  // 🔄 PROCESSO: WiFi check → HTTP GET → Parse JSON → Sort by proximity → Update displa
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\n⚠️  WiFi desconectado - Impossivel atualizar");
    return;
  }
  
  delay(100);
  Serial.println("\n========================================");
  Serial.println("   🔄 SINCRONIZANDO COM FIREBASE");
  Serial.println("========================================");
  
  http.begin(client, FIREBASE_URL);
  int code = http.GET();
  
  if (code == 200) {
    String payload = http.getString();
    
    StaticJsonDocument<4096> doc;
    DeserializationError error = deserializeJson(doc, payload);
    
    if (!error) {
      JsonArray documents = doc["documents"].as<JsonArray>();
      interviewCount = 0;
      
      Serial.printf("\n✅ Sucesso! %d entrevistas encontradas\n\n", documents.size());
      
      time_t now;
      time(&now);
      struct tm* timeinfo = localtime(&now);
      Serial.println("========================================");
      Serial.printf(" ⏰ HORA ATUAL: %02d/%02d/%04d %02d:%02d:%02d BRT\n",
                    timeinfo->tm_mday, timeinfo->tm_mon + 1, timeinfo->tm_year + 1900,
                    timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
      Serial.println("========================================");
      Serial.println();
      
      for (JsonObject doc : documents) {
        if (interviewCount >= 10) break;
        
        JsonObject fields = doc["fields"];
        
        // Extrai o ID do documento do campo "name"
        String fullPath = doc["name"].as<String>();
        int lastSlash = fullPath.lastIndexOf('/');
        String docId = fullPath.substring(lastSlash + 1);
        
        interviews[interviewCount].documentId = docId;
        interviews[interviewCount].candidateName =
          fields["candidateName"]["stringValue"].as<String>();
        interviews[interviewCount].position =
          fields["position"]["stringValue"].as<String>();
        interviews[interviewCount].type =
          fields["type"]["stringValue"].as<String>();
        interviews[interviewCount].duration =
          fields["duration"]["integerValue"].as<int>();
        interviews[interviewCount].dateTime =
          parseFirebaseTimestamp(fields["date"]["timestampValue"].as<const char*>());
        interviews[interviewCount].dismissed = false;
        interviews[interviewCount].snoozed = false;
        
        interviewCount++;
      }
      
      sortInterviewsByProximity();
      
      if (nextInterview) {
        Serial.println("========================================");
        Serial.println("      🎯 PROXIMO ALERTA ATIVO");
        Serial.println("========================================");
        Serial.printf(" 📄 ID: %s\n", nextInterview->documentId.c_str());
        Serial.printf(" 👤 Candidato: %s\n", nextInterview->candidateName.c_str());
        Serial.printf(" 💼 Cargo: %s\n", nextInterview->position.c_str());
        Serial.printf(" ⏱️  Tempo: %s\n", formatTimeUntil(nextInterview->dateTime).c_str());
        Serial.printf(" 📍 Tipo: %s\n", (nextInterview->type == "video" ? "Video chamada" : "Presencial"));
        Serial.printf(" 📅 Data: %s\n", formatDateTime(nextInterview->dateTime).c_str());
        Serial.printf(" ⏳ Duracao: %d min\n", nextInterview->duration);
        Serial.println("========================================");
        
        // Toca som baseado na urgência
        time_t now;
        time(&now);
        long diff = difftime(nextInterview->dateTime, now);
        if (diff < 300) {
          alertSound(2); // Urgente
        } else if (diff < 900) {
          alertSound(1); // Moderado
        } else {
          alertSound(0); // Suave
        }
      } else {
        Serial.println("========================================");
        Serial.println("     ✨ TUDO TRANQUILO POR AQUI");
        Serial.println("   Nenhuma entrevista proxima agendada");
        Serial.println("========================================");
      }
      Serial.println();
      
    } else {
      Serial.println("❌ ERRO: Falha ao processar JSON");
    }
  } else {
    Serial.printf("❌ ERRO HTTP: Codigo %d\n", code);
  }
  
  http.end();
}

// ══════════════════════════════════════════════════════════════════════════════
// 🔘 BUTTONS: INTERAÇÃO COM USUÁRIO
// ══════════════════════════════════════════════════════════════════════════════
// Detecta pressionamentos com debouncing (200ms) e mapeia para ações
void handleButtons() {
  // Botão DISMISS - Marca como CONCLUÍDO no Firebase
  if (digitalRead(BTN_DISMISS) == LOW) {
    if ((millis() - lastDebounce[0]) > debounceDelay) {
      if (nextInterview) {
        Serial.println("\n========================================");
        Serial.println("       🗑️  ALERTA DISPENSADO");
        Serial.println("========================================");
        
        showStatus("Atualizando...");
        
        // Atualiza o status no Firebase
        bool updated = updateInterviewStatus(
          nextInterview->documentId.c_str(), 
          "concluido"
        );
        
        if (updated) {
          Serial.println("✅ Entrevista marcada como CONCLUIDA!");
          nextInterview->dismissed = true;
          sortInterviewsByProximity();
          showDismissed();
          successSound();
        } else {
          Serial.println("❌ Falha ao atualizar Firebase");
          showError();
          errorSound();
        }
        
        delay(2000);
      }
      lastDebounce[0] = millis();
    }
  }
  
  // Botão SNOOZE
  if (digitalRead(BTN_SNOOZE) == LOW) {
    if ((millis() - lastDebounce[1]) > debounceDelay) {
      if (nextInterview) {
        Serial.println("\n========================================");
        Serial.println("     💤 SONECA ATIVADA - 5 MINUTOS");
        Serial.println("========================================");
        nextInterview->snoozed = true;
        nextInterview->snoozeUntil = millis() + 300000;
        sortInterviewsByProximity();
        showSnoozed();
        playTone(600, 300);
        delay(1500);
      } else {
        Serial.println("\n========================================");
        Serial.println("    🔄 ATUALIZACAO MANUAL SOLICITADA");
        Serial.println("========================================");
        showStatus("Atualizando...");
        fetchInterviews();
      }
      lastDebounce[1] = millis();
    }
  }
  
  // Botão CONFIRM
  if (digitalRead(BTN_CONFIRM) == LOW) {
    if ((millis() - lastDebounce[2]) > debounceDelay) {
      if (nextInterview) {
        Serial.println("\n========================================");
        Serial.println("     ✅ PRESENCA CONFIRMADA NA SALA");
        Serial.println("========================================");
        showConfirmed();
        successSound();
        delay(1500);
      }
      lastDebounce[2] = millis();
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════════
// 🚀 SETUP & LOOP: INICIALIZAÇÃO E CICLO PRINCIPAL
// ══════════════════════════════════════════════════════════════════════════════
// Inicializa hardware/software e executa loop principal de sincronização
void setup() {
  delay(1000);
  Serial.begin(115200);
  delay(500);
  
  // HEADER MELHORADO
  Serial.println("\n\n");
  Serial.println("========================================");
  Serial.println("   🎯 COMPASS ASSISTANT PRO");
  Serial.println("========================================");
  Serial.println("  Sistema IoT de Gestao de Entrevistas");
  Serial.println("  com Feedback Multissensorial RGB");
  Serial.println("  + Firebase Status Update");
  Serial.println("========================================");
  Serial.println();
  Serial.println("========================================");
  Serial.println("     🚀 INICIALIZANDO SISTEMA...");
  Serial.println("========================================");
  Serial.println();
  
  // Configura pinos
  pinMode(BTN_SNOOZE, INPUT_PULLUP);
  pinMode(BTN_DISMISS, INPUT_PULLUP);
  pinMode(BTN_CONFIRM, INPUT_PULLUP);
  pinMode(LED_R, OUTPUT);
  pinMode(LED_G, OUTPUT);
  pinMode(LED_B, OUTPUT);
  pinMode(BUZZER, OUTPUT);
  
  // Garante que LED começa desligado
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, LOW);
  
  // LED inicial (amarelo = inicializando)
  currentLedR = 255; // Reset forçado
  currentLedG = 255;
  currentLedB = 0;
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, LOW);
  Serial.println("🎨 LED RGB: 🟡 Amarelo (Inicializando)");
  delay(1000);
  
  client.setInsecure();
  
  Serial.print("📺 Display OLED.......... ");
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("❌ ERRO");
    setLEDRGB(255, 0, 0);
    while (1);
  }
  Serial.println("✅ OK");
  
  showBoot();
  
  Serial.print("💡 LED RGB............... ");
  // Teste sequencial de cores
  Serial.println();
  Serial.print("   Testando Vermelho... ");
  digitalWrite(LED_R, HIGH);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, LOW);
  delay(500);
  Serial.println("OK");
  
  Serial.print("   Testando Verde...... ");
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, HIGH);
  digitalWrite(LED_B, LOW);
  delay(500);
  Serial.println("OK");
  
  Serial.print("   Testando Azul....... ");
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, HIGH);
  delay(500);
  Serial.println("OK");
  
  // Desliga tudo
  digitalWrite(LED_R, LOW);
  digitalWrite(LED_G, LOW);
  digitalWrite(LED_B, LOW);
  Serial.println("✅ LED RGB funcionando!");
  
  Serial.print("🔊 Buzzer................ ");
  playTone(1000, 100);
  Serial.println("✅ OK");
  
  Serial.print("📡 WiFi.................. ");
  setupWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✅ CONECTADO");
    setLEDRGB(0, 255, 0);
  } else {
    Serial.println("❌ ERRO");
    setLEDRGB(255, 0, 0);
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("🕐 Sincronizando hora.... ");
    configTime(-10800, 0, "pool.ntp.org");
    showStatus("Sincronizando...");
    delay(3000);
    Serial.println("✅ OK");
  }
  
  Serial.println();
  fetchInterviews();
  showNextInterview();
  updateLEDStatus();
  lastCheck = millis();
  
  Serial.println();
  Serial.println("========================================");
  Serial.println("     🦊 COMPASS SYSTEM READY! 🦊");
  Serial.println("========================================");
  Serial.println();
}

void loop() {
  handleButtons();
  updateLEDStatus();
  
  if (millis() - lastDisplayUpdate > 100) {
    showNextInterview();
    lastDisplayUpdate = millis();
  }
  
  if (millis() - lastCheck >= CHECK_INTERVAL) {
    Serial.println("\n⏰ AUTO-REFRESH (Atualizacao automatica a cada 2min)");
    fetchInterviews();
    showNextInterview();
    lastCheck = millis();
  }
  
  delay(50);
}