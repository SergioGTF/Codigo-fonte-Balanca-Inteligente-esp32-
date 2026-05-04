#include <Arduino.h>
#include "HX711.h"

// ================= PINOS =================
#define DT1 19
#define SCK1 18

#define DT2 17
#define SCK2 16

#define DT3 4
#define SCK3 2

#define DT4 15
#define SCK4 13

// ================= HX711 =================
HX711 s1, s2, s3, s4;

// ================= CALIBRAÇÃO =================
// SUBSTITUIR DEPOIS COM OS VALORES REAIS
float cal1 = -8000.0;
float cal2 = -8000.0;
float cal3 = -8000.0;
float cal4 = -8000.0;

// ================= FILTRO =================
float f1=0, f2=0, f3=0, f4=0;

float filtro(float valor, float &anterior) {
  float alpha = 0.25;
  float out = alpha * valor + (1 - alpha) * anterior;
  anterior = out;
  return out;
}

// ================= LEITURA SEGURA =================
float ler(HX711 &s) {
  if (!s.is_ready()) return 0;

  float soma = 0;
  int n = 0;

  for (int i = 0; i < 10; i++) {
    if (s.is_ready()) {
      soma += s.get_units();
      n++;
    }
    delay(3);
  }

  if (n == 0) return 0;

  float media = soma / n;

  if (abs(media) < 0.05) return 0;

  return media;
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  delay(1000);

  s1.begin(DT1, SCK1);
  s2.begin(DT2, SCK2);
  s3.begin(DT3, SCK3);
  s4.begin(DT4, SCK4);

  s1.set_scale(cal1);
  s2.set_scale(cal2);
  s3.set_scale(cal3);
  s4.set_scale(cal4);

  Serial.println("Aguarde... TARANDO");

  delay(2000);

  s1.tare();
  s2.tare();
  s3.tare();
  s4.tare();

  Serial.println("Pronto para medir");
}

// ================= LOOP =================
void loop() {

  float p1 = filtro(ler(s1), f1);
  float p2 = filtro(ler(s2), f2);
  float p3 = filtro(ler(s3), f3);
  float p4 = filtro(ler(s4), f4);

  float total = p1 + p2 + p3 + p4;

  // proteção contra valores inválidos
  if (total < 0) total = 0;
  if (total > 100) total = 0;

  // ================= DEBUG =================
  Serial.println("==========");
  Serial.print("S1: "); Serial.println(p1, 2);
  Serial.print("S2: "); Serial.println(p2, 2);
  Serial.print("S3: "); Serial.println(p3, 2);
  Serial.print("S4: "); Serial.println(p4, 2);

  Serial.print("TOTAL: ");
  Serial.print(total, 2);
  Serial.println(" kg");

  // ================= JSON =================
  Serial.print("{");
  Serial.print("\"s1\":"); Serial.print(p1,2); Serial.print(",");
  Serial.print("\"s2\":"); Serial.print(p2,2); Serial.print(",");
  Serial.print("\"s3\":"); Serial.print(p3,2); Serial.print(",");
  Serial.print("\"s4\":"); Serial.print(p4,2); Serial.print(",");
  Serial.print("\"peso\":"); Serial.print(total,2);
  Serial.println("}");

  delay(500);
}