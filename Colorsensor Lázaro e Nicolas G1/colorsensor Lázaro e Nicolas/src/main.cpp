#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <BH1750.h>

// ===== DEBUG =====
#define DEBUG 1
#define DBG(...) do { if (DEBUG) Serial.printf(__VA_ARGS__); } while (0)

// ======================= Pinos TCS3200 =======================
const int S0 = 5, S1 = 18, S2 = 21, S3 = 22, OUT_PIN = 4, LED_CTRL = 19;

// ======================= LED RGB (4 pinos) ===================
// (use pinos PWM; 35 é só entrada!)
const int LED_R_PIN = 25;
const int LED_G_PIN = 32;
const int LED_B_PIN = 33;
const bool COMMON_ANODE = false;
const int CH_R = 0, CH_G = 1, CH_B = 2;
const int PWM_FREQ = 5000, PWM_RES = 8;

// ======================= Wi-Fi / MQTT ========================
const char* ssid     = "AMF-CORP";
const char* password = "@MF$4515";

const char* mqtt_server = "test.mosquitto.org";
const int   mqtt_port   = 1883;

// ---- Tópicos
const char* TOPIC_COLOR = "LazaroNicolas/ambient/color";
const char* TOPIC_LUX   = "LazaroNicolas/ambient/lux";
const char* TOPIC_LED   = "LazaroNicolas/ambient/led";
const char* TOPIC_STAT  = "LazaroNicolas/ambient/status";
const char* TOPIC_CMD   = "LazaroNicolas/ambient/cmd";   // comandos externos (JSON {"led":[r,g,b]})

WiFiClient espClient;
PubSubClient mqtt(espClient);

// =================== BH1750 (I2C) ============================
BH1750 lightMeter;
TwoWire I2Cbh(0);
const int SDA_BH = 26, SCL_BH = 27;

// =================== Calibração TCS3200 ======================
float Fmin_R=200, Fmax_R=2500, Fmin_G=200, Fmax_G=2500, Fmin_B=200, Fmax_B=2500;

// =================== Estado / Publish control ================
uint8_t lastR=0,lastG=0,lastB=0;
float   lastLux=-1.0f;
unsigned long lastPublish=0;
const unsigned long HEARTBEAT_MS = 10000;
const int    RGB_DELTA = 10;
const float  LUX_DELTA_ABS = 10.0f;
const float  LUX_DELTA_REL = 0.10f;

// ======= Controle de troca do LED (hold de 1s) =======
uint8_t curR = 0, curG = 0, curB = 0; // aplicada de fato
unsigned long lastLedApply = 0;
const unsigned long LED_HOLD_MS = 1000;

// ---------------------- TCS3200 helpers ----------------------
static inline void setFilter(int s2,int s3){ digitalWrite(S2,s2); digitalWrite(S3,s3); }
static float measureFreqHz(uint32_t tout_us=50000){
  unsigned long tL = pulseIn(OUT_PIN, LOW,  tout_us);
  unsigned long tH = pulseIn(OUT_PIN, HIGH, tout_us);
  if (!tL || !tH) return 0.0f;
  return 1000000.0f / (float)(tL + tH);
}
static float avgFreqForFilter(int s2,int s3,int samples=3){
  setFilter(s2,s3); delay(20);
  float acc=0; int ok=0;
  for(int i=0;i<samples;i++){ float f=measureFreqHz(); if(f>0){acc+=f;ok++;} delay(5); }
  return ok? acc/ok : 0.0f;
}
static uint8_t mapFreqTo8(float f,float fmin,float fmax){
  if (f<=0) return 0; if (f<fmin) f=fmin; if (f>fmax) f=fmax;
  float x=(f-fmin)/(fmax-fmin);
  int v=(int)roundf(x*255.0f);
  if(v<0) v=0; else if(v>255) v=255;
  return (uint8_t)v;
}
static void rgbToHsv(uint8_t R,uint8_t G,uint8_t B,float &H,float &S,float &V){
  float r=R/255.0f, g=G/255.0f, b=B/255.0f;
  float maxv=max(r,max(g,b)), minv=min(r,min(g,b)), d=maxv-minv;
  V=maxv; S=(maxv==0.0f)?0.0f:(d/maxv);
  if(d==0.0f){ H=0; return; }
  if(maxv==r) H=fmodf(((g-b)/d),6.0f);
  else if(maxv==g) H=((b-r)/d)+2.0f;
  else H=((r-g)/d)+4.0f;
  H*=60.0f; if(H<0) H+=360.0f;
}
static const char* classifyColor(float H,float S,float V){
  if(V<0.08f) return "preto";
  if(S<0.12f) return (V>0.85f)?"branco":"cinza";
  if(H<15||H>=345) return "vermelho";
  if(H<45)  return "laranja";
  if(H<70)  return "amarelo";
  if(H<170) return "verde";
  if(H<200) return "ciano";
  if(H<255) return "azul";
  if(H<290) return "anil";
  if(H<345) return "magenta";
  return "desconhecido";
}
static void setLedRGB(uint8_t r,uint8_t g,uint8_t b){
  if (COMMON_ANODE){ r=255-r; g=255-g; b=255-b; }
  ledcWrite(CH_R, r); ledcWrite(CH_G, g); ledcWrite(CH_B, b);
}

// -------------------- Wi-Fi / MQTT ---------------------------
void wifiConnect(){
  DBG("\n[WiFi] Conectando \"%s\"...\n", ssid);
  WiFi.mode(WIFI_STA); WiFi.begin(ssid, password);
  uint32_t t0=millis();
  while(WiFi.status()!=WL_CONNECTED){
    delay(300); Serial.print(".");
    if(millis()-t0>15000){ DBG("\n[WiFi] timeout, retry...\n"); t0=millis(); }
  }
  DBG("\n[WiFi] OK: %s\n", WiFi.localIP().toString().c_str());
}

// --- parser tolerante do payload {"led":[r,g,b]}
static bool parseLedPayload(const char* s, int &r, int &g, int &b){
  // tenta com espaços opcionais
  if (sscanf(s, " { \"led\" : [ %d , %d , %d ] } ", &r,&g,&b) == 3) return true;
  if (sscanf(s, "{\"led\":[%d,%d,%d]}", &r,&g,&b) == 3) return true;
  // fallback simples: procura os colchetes e lê números
  const char* lb = strchr(s, '[');
  const char* rb = lb ? strchr(lb, ']') : nullptr;
  if(!lb || !rb) return false;
  String inside = String(lb+1); inside.remove(rb - lb - 1);
  int vals[3]={0,0,0}, idx=0; long v=0; bool neg=false; bool num=false;
  for (size_t i=0;i<inside.length() && idx<3;i++){
    char c = inside[i];
    if (c=='-' && !num){ neg=true; continue; }
    if (c>='0' && c<='9'){ num=true; v = v*10 + (c-'0'); continue; }
    if (num){ vals[idx++] = neg? -(int)v:(int)v; v=0; neg=false; num=false; }
  }
  if (num && idx<3){ vals[idx++] = neg? -(int)v:(int)v; }
  if (idx==3){ r=vals[0]; g=vals[1]; b=vals[2]; return true; }
  return false;
}

void onMqttMessage(char* topic, byte* payload, unsigned int len){
  DBG("[MQTT] RX topic=%s len=%u\n", topic, len);
  // copia payload para buffer C-string
  char buf[256];
  unsigned int n = (len < sizeof(buf)-1) ? len : sizeof(buf)-1;
  memcpy(buf, payload, n); buf[n]=0;
  DBG("[MQTT] payload=%s\n", buf);

  if (strcmp(topic, TOPIC_CMD) == 0){
    int r,g,b;
    if (parseLedPayload(buf, r,g,b)){
      r = constrain(r,0,255); g = constrain(g,0,255); b = constrain(b,0,255);
      DBG("[CMD] LED <- (%d,%d,%d)\n", r,g,b);
      setLedRGB((uint8_t)r,(uint8_t)g,(uint8_t)b);
      curR = r; curG = g; curB = b;
      lastLedApply = millis();
      char out[96];
      snprintf(out,sizeof(out), "{\"led_rgb\":[%d,%d,%d],\"ts\":%lu}", r,g,b,millis());
      mqtt.publish(TOPIC_LED, out, true); // retained
      DBG("[MQTT] echo LED state -> %s\n", out);
    } else {
      DBG("[CMD] payload inválido (esperado {\"led\":[r,g,b]})\n");
    }
  }
}

void mqttReconnect(){
  if(mqtt.connected()) return;
  DBG("[MQTT] Conectando %s:%d ...\n", mqtt_server, mqtt_port);
  while(!mqtt.connected()){
    if(mqtt.connect("ESP32-AmbientMatch")){
      DBG("[MQTT] conectado.\n");
      bool ok = mqtt.subscribe(TOPIC_CMD); // QoS 0
      DBG("[MQTT] subscribe %s -> %s\n", TOPIC_CMD, ok ? "OK" : "FAIL");
      mqtt.publish(TOPIC_STAT, "{\"status\":\"online\"}", true);
      break;
    } else {
      DBG("[MQTT] falha (state=%d). retry...\n", mqtt.state());
      delay(1500);
    }
  }
}

void publishLux(float lux){
  char buf[64];
  snprintf(buf,sizeof(buf), "{\"lux\":%.2f,\"ts\":%lu}", lux, millis());
  mqtt.publish(TOPIC_LUX, buf, true);
  DBG("[MQTT] lux -> %s\n", buf);
}
void publishColor(const char* name,uint8_t R,uint8_t G,uint8_t B,float H,float S,float V,
                  float fR,float fG,float fB){
  char payload[256];
  snprintf(payload,sizeof(payload),
    "{\"rgb\":[%u,%u,%u],\"hsv\":{\"h\":%.0f,\"s\":%.2f,\"v\":%.2f},"
    "\"freq\":{\"r\":%.0f,\"g\":%.0f,\"b\":%.0f},\"color\":\"%s\",\"ts\":%lu}",
    R,G,B,H,S,V,fR,fG,fB,name,millis());
  mqtt.publish(TOPIC_COLOR, payload, true);
  DBG("[MQTT] color -> %s\n", payload);
}
void publishLed(uint8_t r,uint8_t g,uint8_t b){
  char buf[96];
  snprintf(buf,sizeof(buf), "{\"led_rgb\":[%u,%u,%u],\"ts\":%lu}", r,g,b,millis());
  mqtt.publish(TOPIC_LED, buf, true);
  DBG("[MQTT] led -> %s\n", buf);
}

// ============================ SETUP ===========================
void setup(){
  Serial.begin(115200); delay(200);
  Serial.println("\n=== Ambient Match: TCS3200 + BH1750 + LED + MQTT ===");

  // TCS3200
  pinMode(S0,OUTPUT); pinMode(S1,OUTPUT); pinMode(S2,OUTPUT); pinMode(S3,OUTPUT);
  pinMode(OUT_PIN,INPUT); pinMode(LED_CTRL,OUTPUT);
  digitalWrite(LED_CTRL,HIGH);
  digitalWrite(S0,HIGH); digitalWrite(S1,LOW); // ~20%

  // LED RGB
  ledcSetup(CH_R,PWM_FREQ,PWM_RES);
  ledcSetup(CH_G,PWM_FREQ,PWM_RES);
  ledcSetup(CH_B,PWM_FREQ,PWM_RES);
  ledcAttachPin(LED_R_PIN,CH_R);
  ledcAttachPin(LED_G_PIN,CH_G);
  ledcAttachPin(LED_B_PIN,CH_B);
  setLedRGB(0,0,0);
  curR = curG = curB = 0; lastLedApply = millis();

  // BH1750
  I2Cbh.begin(SDA_BH, SCL_BH, 400000);
  bool bhOk = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &I2Cbh);
  if(!bhOk) Serial.println("[BH1750] ERRO (tente ADDR 0x5C).");
  else      Serial.println("[BH1750] OK.");

  // Wi-Fi / MQTT
  wifiConnect();
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(onMqttMessage);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(5);
  mqtt.setBufferSize(512); // margem para payloads
}

// ============================= LOOP ===========================
void loop(){
  if(WiFi.status()!=WL_CONNECTED) wifiConnect();
  if(!mqtt.connected()) mqttReconnect();
  mqtt.loop();

  // ---- TCS3200: medir cor ----
  float fR = avgFreqForFilter(LOW,LOW);
  float fB = avgFreqForFilter(LOW,HIGH);
  float fG = avgFreqForFilter(HIGH,HIGH);

  uint8_t R = mapFreqTo8(fR,Fmin_R,Fmax_R);
  uint8_t G = mapFreqTo8(fG,Fmin_G,Fmax_G);
  uint8_t B = mapFreqTo8(fB,Fmin_B,Fmax_B);

  float H,S,V; rgbToHsv(R,G,B,H,S,V);
  const char* name = classifyColor(H,S,V);

  // ---- BH1750: lux ----
  float lux = lightMeter.readLightLevel();

  // ---- Brilho do LED pelo lux (alvo)
  float k = lux / 800.0f; if(k<0.12f) k=0.12f; if(k>1.0f) k=1.0f;
  uint8_t rOut = (uint8_t)(R * k);
  uint8_t gOut = (uint8_t)(G * k);
  uint8_t bOut = (uint8_t)(B * k);

  // ===== Hold 1s =====
  bool wouldChange =
    (abs((int)rOut - (int)curR) >= RGB_DELTA) ||
    (abs((int)gOut - (int)curG) >= RGB_DELTA) ||
    (abs((int)bOut - (int)curB) >= RGB_DELTA);

  unsigned long now = millis();
  if (wouldChange && (now - lastLedApply) >= LED_HOLD_MS) {
    setLedRGB(rOut, gOut, bOut);
    curR = rOut; curG = gOut; curB = bOut;
    lastLedApply = now;
    publishLed(curR, curG, curB); // estado real aplicado
  }

  // ---- Serial
  Serial.printf("LUX: %.1f | RGB(%3u,%3u,%3u)->TARGET(%3u,%3u,%3u) | LED(%3u,%3u,%3u) | COR: %s\n",
                lux, R,G,B, rOut,gOut,bOut, curR,curG,curB, name);

  // ---- Publicação condicional + heartbeat ----
  bool colorChanged =
    (abs((int)R - (int)lastR) >= RGB_DELTA) ||
    (abs((int)G - (int)lastG) >= RGB_DELTA) ||
    (abs((int)B - (int)lastB) >= RGB_DELTA);

  bool luxChanged =
    (lastLux < 0) ||
    (fabs(lux - lastLux) >= LUX_DELTA_ABS) ||
    (fabs(lux - lastLux) >= max(LUX_DELTA_ABS, lastLux * LUX_DELTA_REL));

  bool heartbeat = (now - lastPublish >= HEARTBEAT_MS);

  if(colorChanged || luxChanged || heartbeat){
    if(luxChanged || heartbeat) publishLux(lux);
    if(colorChanged || heartbeat) publishColor(name,R,G,B,H,S,V,fR,fG,fB);
    if(heartbeat) publishLed(curR, curG, curB);
    lastR = R; lastG = G; lastB = B; lastLux = lux; lastPublish = now;
  }

  delay(250);
}
