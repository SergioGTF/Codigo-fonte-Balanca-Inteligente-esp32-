#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <LiquidCrystal.h>
#include "HX711.h"

// ================= UUIDs =================
#define SERVICE_UUID "afb5ae53-0d61-460d-a5e9-4c7aa14aa064"
#define CHAR_NOTIFY_UUID "78dbc0a6-3554-44ba-9496-446f232a2fbc"
#define CHAR_WRITE_UUID "a1b2c3d4-e5f6-7890-abcd-ef1234567890"

// ================= CONFIGURAÇÕES =================
#define PESO_MAXIMO_KG 80.0f // 4 células × 20 kg
#define PESO_MINIMO_KG 0.05f
#define NOTIFY_INTERVAL_MS 500
#define WDT_TIMEOUT_S 10
#define LED_STATUS_PIN 14
#define BUTTON_PRODUTO_PIN 0 // Boot button — troca produto
#define DEBOUNCE_MS 200

// ================= PINOS — 1 MÓDULO HX711 =================
// Ligue as 4 células em ponte Wheatstone (veja diagrama acima).
// Apenas 2 pinos são necessários:
#define DT_PIN 19
#define SCK_PIN 18

// ================= LCD (sem I2C, 4 bits) =================
//        RS   EN   D4   D5   D6   D7
LiquidCrystal lcd(5, 23, 25, 26, 27, 32);

// ================= TABELA DE PRODUTOS =================
struct Produto
{
  const char *nome;
  float precoPorKg;
};

const Produto PRODUTOS[] = {
    {"Acai", 6.00f},
    {"Dende", 3.00f},
    {"Cacau", 14.00f},
    {"Farinha", 4.50f},
    {"Mandioca", 2.00f},
};
const int NUM_PRODUTOS = 5;
int produtoAtual = 0;
unsigned long lastBotao = 0;

// 35 % do valor bruto como custo de produção estimado
#define CUSTO_PERCENTUAL 0.35f

// ================= OBJETOS =================
HX711 sensor; // único módulo — lê a ponte completa das 4 células
Preferences prefs;
float calFator = -8000.0f;
float filtroVal = 0.0f;

// ================= BLE =================
BLEServer *pServer = nullptr;
BLECharacteristic *pCharNotify = nullptr;
BLECharacteristic *pCharWrite = nullptr;
bool deviceConnected = false;
bool oldDeviceConnected = false;

// ================= NVS — CALIBRAÇÃO =================
void carregarCalibracao()
{
  prefs.begin("balanca", true);
  calFator = prefs.getFloat("cal", -8000.0f);
  prefs.end();
  Serial.printf("  Fator de calibracao: %.2f\n", calFator);
}

void salvarCalibracao()
{
  prefs.begin("balanca", false);
  prefs.putFloat("cal", calFator);
  prefs.end();
}

// ================= FILTRO EMA =================
float filtroEMA(float valor, float &anterior)
{
  const float alpha = 0.25f;
  float out = alpha * valor + (1.0f - alpha) * anterior;
  anterior = out;
  return out;
}

// ================= LEITURA DO SENSOR =================
float lerSensor()
{
  if (!sensor.is_ready())
  {
    Serial.println("[WARN] Sensor nao pronto");
    return 0.0f;
  }
  float soma = 0.0f;
  int n = 0;
  for (int i = 0; i < 10; i++)
  {
    if (sensor.is_ready())
    {
      soma += sensor.get_units();
      n++;
    }
    delay(3);
  }
  if (n == 0)
    return 0.0f;
  float media = soma / (float)n;
  return (fabsf(media) < PESO_MINIMO_KG) ? 0.0f : media;
}

// ================= CÁLCULO FINANCEIRO =================
struct Calculo
{
  float valorBruto;
  float custo;
  float lucro;
};

Calculo calcular(float pesoKg)
{
  Calculo c;
  c.valorBruto = pesoKg * PRODUTOS[produtoAtual].precoPorKg;
  c.custo = c.valorBruto * CUSTO_PERCENTUAL;
  c.lucro = c.valorBruto - c.custo;
  return c;
}

// ================= LCD =================
void lcdInicio()
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  Agro Agenda   ");
  lcd.setCursor(0, 1);
  lcd.print("  Iniciando...  ");
}

void lcdTarando()
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  Agro Agenda   ");
  lcd.setCursor(0, 1);
  lcd.print("  Tarando...    ");
}

void lcdPronto()
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("  Agro Agenda   ");
  lcd.setCursor(0, 1);
  lcd.print("  Pronto!       ");
}

void lcdMostrarProduto()
{
  char buf[17];
  snprintf(buf, sizeof(buf), "%-8s R$%.2f",
           PRODUTOS[produtoAtual].nome,
           PRODUTOS[produtoAtual].precoPorKg);
  lcd.setCursor(0, 0);
  lcd.print(buf);
  lcd.setCursor(0, 1);
  lcd.print("Produto trocado ");
}

// Linha 0: "Produto  R$X.XX"
// Linha 1: "XX.Xkg R$XXX.XX" ou "BLE:Aguardando  "
void lcdAtualizar(float total)
{
  char linha0[17];
  snprintf(linha0, sizeof(linha0), "%-8s R$%.2f",
           PRODUTOS[produtoAtual].nome,
           PRODUTOS[produtoAtual].precoPorKg);
  lcd.setCursor(0, 0);
  lcd.print(linha0);

  char linha1[17];
  if (total < PESO_MINIMO_KG)
  {
    snprintf(linha1, sizeof(linha1), "BLE:%-12s",
             deviceConnected ? "Conectado" : "Aguardando");
  }
  else
  {
    float vb = total * PRODUTOS[produtoAtual].precoPorKg;
    snprintf(linha1, sizeof(linha1), "%5.1fkg R$%6.2f", total, vb);
  }
  lcd.setCursor(0, 1);
  lcd.print(linha1);
}

// ================= LED =================
void ledStatus(bool conectado)
{
  static unsigned long lastBlink = 0;
  static bool ledState = false;
  if (conectado)
  {
    digitalWrite(LED_STATUS_PIN, HIGH);
    return;
  }
  if (millis() - lastBlink > 800)
  {
    ledState = !ledState;
    digitalWrite(LED_STATUS_PIN, ledState);
    lastBlink = millis();
  }
}

// ================= SERIAL =================
void serialPrintDados(float total, const Calculo &c, const char *status, unsigned long ts)
{
  Serial.println("==========================================");
  Serial.printf("  Produto  : %s @ R$ %.2f/kg\n",
                PRODUTOS[produtoAtual].nome, PRODUTOS[produtoAtual].precoPorKg);
  Serial.println("  ----------------------------------------");
  Serial.printf("  TOTAL    : %7.2f kg  [%s]\n", total, status);
  Serial.printf("  Bruto    : R$ %.2f\n", c.valorBruto);
  Serial.printf("  Custo    : R$ %.2f (35%%)\n", c.custo);
  Serial.printf("  Lucro    : R$ %.2f\n", c.lucro);
  Serial.printf("  BLE      : %s\n", deviceConnected ? "Conectado" : "Aguardando");
  Serial.printf("  Uptime   : %lu ms\n", ts);
  Serial.println("==========================================");
}

// ================= BLE NOTIFY =================
// O app espera os campos s1–s4 para exibir o grid de sensores.
// Com 1 HX711 lendo a ponte completa, distribuímos o peso total
// igualmente entre os 4 campos (não temos leituras individuais).
void bleNotify(float total, const Calculo &c, const char *status, unsigned long ts)
{
  if (!deviceConnected || !pCharNotify)
    return;

  float quarto = total / 4.0f; // distribuição igual para o grid do app

  char json[220];
  snprintf(json, sizeof(json),
           "{\"s1\":%.2f,\"s2\":%.2f,\"s3\":%.2f,\"s4\":%.2f,"
           "\"peso\":%.2f,\"produto\":\"%s\",\"preco_kg\":%.2f,"
           "\"valor_bruto\":%.2f,\"lucro\":%.2f,"
           "\"status\":\"%s\",\"ts\":%lu}\n",
           quarto, quarto, quarto, quarto, total,
           PRODUTOS[produtoAtual].nome,
           PRODUTOS[produtoAtual].precoPorKg,
           c.valorBruto, c.lucro,
           status, ts);

  size_t len = strlen(json);
  const size_t CHUNK = 180;

  if (len <= CHUNK)
  {
    pCharNotify->setValue((uint8_t *)json, len);
    pCharNotify->notify();
  }
  else
  {
    for (size_t i = 0; i < len; i += CHUNK)
    {
      size_t n = min(CHUNK, len - i);
      pCharNotify->setValue((uint8_t *)(json + i), n);
      pCharNotify->notify();
      delay(20);
    }
  }
}

// ================= CALLBACKS BLE =================
class ServerCallbacks : public BLEServerCallbacks
{
  void onConnect(BLEServer *) override
  {
    deviceConnected = true;
    Serial.println("[BLE] App conectado");
    lcd.setCursor(0, 1);
    lcd.print("BLE:Conectado   ");
  }
  void onDisconnect(BLEServer *) override
  {
    deviceConnected = false;
    Serial.println("[BLE] App desconectado");
    lcd.setCursor(0, 1);
    lcd.print("BLE:Aguardando  ");
  }
};

class WriteCallbacks : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *pChar) override
  {
    std::string val = pChar->getValue();
    if (val.empty())
      return;
    Serial.printf("[CMD] %s\n", val.c_str());

    if (val == "tare")
    {
      // ── Tara o sensor único ───────────────────────────────────────
      lcdTarando();
      sensor.tare();
      filtroVal = 0.0f;
      Serial.println("[CMD] Tara OK");
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("  Agro Agenda   ");
      lcd.setCursor(0, 1);
      lcd.print("  Tara OK!      ");
      delay(800);
    }
    else if (val == "zero")
    {
      filtroVal = 0.0f;
      Serial.println("[CMD] Filtro zerado");
    }
    else if (val.rfind("produto:", 0) == 0)
    {
      // ── Troca produto: "produto:Acai" ─────────────────────────────
      String nome = String(val.substr(8).c_str());
      for (int i = 0; i < NUM_PRODUTOS; i++)
      {
        if (nome.equalsIgnoreCase(PRODUTOS[i].nome))
        {
          produtoAtual = i;
          lcdMostrarProduto();
          Serial.printf("[CMD] Produto: %s @ R$ %.2f/kg\n",
                        PRODUTOS[i].nome, PRODUTOS[i].precoPorKg);
          break;
        }
      }
    }
    else if (val.rfind("calibrate:", 0) == 0)
    {
      // ── Calibração: "calibrate:10.000" (peso de referência em kg) ──
      // Coloque o peso de referência na balança antes de enviar.
      float pesoRef = atof(val.substr(10).c_str());
      if (pesoRef > 0.0f)
      {
        long raw = sensor.get_value(20); // 20 amostras para estabilidade
        if (raw != 0)
        {
          calFator = (float)raw / pesoRef;
          sensor.set_scale(calFator);
          salvarCalibracao();
          Serial.printf("[CMD] Calibrado: ref=%.3f kg | fator=%.2f\n",
                        pesoRef, calFator);
          lcd.setCursor(0, 1);
          lcd.print("Cal OK!         ");
          delay(800);
        }
        else
        {
          Serial.println("[CMD] ERRO: leitura zero — verifique as celulas");
        }
      }
    }
  }
};

// ================= SETUP =================
void setup()
{
  Serial.begin(115200);
  delay(1000);

  pinMode(LED_STATUS_PIN, OUTPUT);
  pinMode(BUTTON_PRODUTO_PIN, INPUT_PULLUP);

  lcd.begin(16, 2);
  lcdInicio();

  Serial.println("\n========== AgroAgenda Balanca (1x HX711) ==========");
  Serial.println("  4 celulas de carga em ponte Wheatstone completa");

  // Watchdog
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);

  // Calibração salva em NVS
  carregarCalibracao();

  // HX711 — 1 módulo conectado à ponte das 4 células
  sensor.begin(DT_PIN, SCK_PIN);
  sensor.set_scale(calFator);

  lcdTarando();
  delay(2000);
  sensor.tare();
  filtroVal = 0.0f;
  Serial.println("  Sensor pronto e tarado");

  // BLE
  BLEDevice::init("AgroAgenda-Balanca");
  BLEDevice::setMTU(185);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *svc = pServer->createService(SERVICE_UUID);

  pCharNotify = svc->createCharacteristic(CHAR_NOTIFY_UUID,
                                          BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  pCharNotify->addDescriptor(new BLE2902());

  pCharWrite = svc->createCharacteristic(CHAR_WRITE_UUID,
                                         BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  pCharWrite->setCallbacks(new WriteCallbacks());

  svc->start();

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();

  lcdPronto();
  delay(800);
  lcdAtualizar(0.0f);

  Serial.printf("  Produto inicial: %s @ R$ %.2f/kg\n",
                PRODUTOS[produtoAtual].nome, PRODUTOS[produtoAtual].precoPorKg);
  Serial.println("  Sistema pronto\n");
}

// ================= LOOP =================
unsigned long lastNotify = 0;

void loop()
{
  esp_task_wdt_reset();
  ledStatus(deviceConnected);

  // ── Botão: cicla produtos (antes do throttle para resposta imediata) ──
  if (digitalRead(BUTTON_PRODUTO_PIN) == LOW && millis() - lastBotao > DEBOUNCE_MS)
  {
    lastBotao = millis();
    produtoAtual = (produtoAtual + 1) % NUM_PRODUTOS;
    lcdMostrarProduto();
    Serial.printf("[BTN] Produto: %s @ R$ %.2f/kg\n",
                  PRODUTOS[produtoAtual].nome, PRODUTOS[produtoAtual].precoPorKg);
    delay(700);
  }

  // ── Reconexão BLE ──
  if (!deviceConnected && oldDeviceConnected)
  {
    delay(400);
    pServer->startAdvertising();
    Serial.println("[BLE] Advertising reiniciado");
    oldDeviceConnected = false;
  }
  if (deviceConnected && !oldDeviceConnected)
    oldDeviceConnected = true;

  // ── Throttle ──
  unsigned long now = millis();
  if (now - lastNotify < NOTIFY_INTERVAL_MS)
    return;
  lastNotify = now;

  // ── Leitura única — ponte das 4 células ──
  float total = filtroEMA(lerSensor(), filtroVal);
  if (total < 0)
    total = 0.0f;
  if (total > PESO_MAXIMO_KG)
    total = PESO_MAXIMO_KG;

  const char *status = (total > PESO_MINIMO_KG) ? "ok" : "vazio";
  Calculo c = calcular(total);

  lcdAtualizar(total);
  serialPrintDados(total, c, status, now);
  bleNotify(total, c, status, now);
}