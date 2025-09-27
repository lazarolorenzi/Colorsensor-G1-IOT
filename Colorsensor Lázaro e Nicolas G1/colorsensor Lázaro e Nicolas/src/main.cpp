#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <BH1750.h>

// ===== DEBUG =====
#define DEBUG 1
#define DBG(...) do { if (DEBUG) Serial.printf(__VA_ARGS__); } while (0)

// ======================= Pinos TCS3200 =======================
const int S0 = 5, S1 = 18, S2 = 21, S3 = 22, OUT_PIN = 4, LED_CTRL = 19;

// ======================= LED RGB (4 pinos) ===================
// OBS: GPIO35 é somente entrada. Use pinos com PWM:
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
const char* TOPIC_CMD   = "LazaroNicolas/ambient/cmd";   // comandos externos (opcional)

WiFiClient espClient;
PubSubClient mqtt(espClient);

// =================== BH1750 (I2C) ============================
BH1750 lightMeter;
TwoWire I2Cbh(0);
const int SDA_BH = 26, SCL_BH = 27;

// =================== Calibração TCS3200 ======================
float Fmin_R=200, Fmax_R=2500, Fmin_G=200, Fmax_G=2500, Fmin_B=200, Fmax_B=2500;

// =================== WebServer / API =========================
WebServer server(80);

// =================== Estado / Publish control ================
uint8_t lastR=0,lastG=0,lastB=0;
float   lastLux=-1.0f;
unsigned long lastPublish=0;
const unsigned long HEARTBEAT_MS = 10000;
const int    RGB_DELTA = 10;
const float  LUX_DELTA_ABS = 10.0f;
const float  LUX_DELTA_REL = 0.10f;

// ======= Controle de troca do LED (hold de 1s) =======
uint8_t curR = 0, curG = 0, curB = 0;           // última cor realmente aplicada no LED
unsigned long lastLedApply = 0;                  // instante da última troca aplicada
const unsigned long LED_HOLD_MS = 1000;          // segurar 1 segundo antes de trocar

// ---- buffers com os últimos JSONs (para a API / dashboard) --
String lastLuxJson   = "{}";
String lastColorJson = "{}";
String lastLedJson   = "{}";
unsigned long lastMsgMillis = 0;

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
  float x=(f-fmin)/(fmax-fmin); int v=(int)roundf(x*255.0f);
  return (uint8_t)(v<0?0:(v>255?255:v));
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

void onMqttMessage(char* topic, byte* payload, unsigned int len){
  // Se você quiser aceitar comandos MQTT externos: {"led":[r,g,b]}
  if (String(topic) == TOPIC_CMD){
    String msg; msg.reserve(len);
    for(unsigned int i=0;i<len;i++) msg += (char)payload[i];
    int r,g,b;
    if (sscanf(msg.c_str(), "{\"led\":[%d,%d,%d]}", &r,&g,&b) == 3){
      r = constrain(r,0,255); g = constrain(g,0,255); b = constrain(b,0,255);
      setLedRGB((uint8_t)r,(uint8_t)g,(uint8_t)b);
      curR = r; curG = g; curB = b;
      lastLedApply = millis();
      char buf[96];
      snprintf(buf,sizeof(buf), "{\"led_rgb\":[%d,%d,%d],\"ts\":%lu}", r,g,b,millis());
      lastLedJson = String(buf);
      mqtt.publish(TOPIC_LED, buf, true);
    }
  }
}

void mqttReconnect(){
  if(mqtt.connected()) return;
  DBG("[MQTT] Conectando %s:%d ...\n", mqtt_server, mqtt_port);
  while(!mqtt.connected()){
    if(mqtt.connect("ESP32-AmbientMatch")){
      DBG("[MQTT] conectado.\n");
      mqtt.subscribe(TOPIC_CMD);
      mqtt.publish(TOPIC_STAT, "{\"status\":\"online\"}", true);
      break;
    } else {
      DBG("[MQTT] falha (state=%d). retry...\n", mqtt.state());
      delay(1500);
    }
  }
}

// -------------------- Publish + cache p/ API -----------------
void publishLux(float lux){
  char buf[64];
  snprintf(buf,sizeof(buf), "{\"lux\":%.2f,\"ts\":%lu}", lux, millis());
  lastLuxJson = String(buf);
  mqtt.publish(TOPIC_LUX, buf, true);
  lastMsgMillis = millis();
  DBG("[MQTT] lux -> %s\n", buf);
}
void publishColor(const char* name,uint8_t R,uint8_t G,uint8_t B,float H,float S,float V,
                  float fR,float fG,float fB){
  char payload[256];
  snprintf(payload,sizeof(payload),
    "{\"rgb\":[%u,%u,%u],\"hsv\":{\"h\":%.0f,\"s\":%.2f,\"v\":%.2f},"
    "\"freq\":{\"r\":%.0f,\"g\":%.0f,\"b\":%.0f},\"color\":\"%s\",\"ts\":%lu}",
    R,G,B,H,S,V,fR,fG,fB,name,millis());
  lastColorJson = String(payload);
  mqtt.publish(TOPIC_COLOR, payload, true);
  lastMsgMillis = millis();
  DBG("[MQTT] color -> %s\n", payload);
}
void publishLed(uint8_t r,uint8_t g,uint8_t b){
  char buf[96];
  snprintf(buf,sizeof(buf), "{\"led_rgb\":[%u,%u,%u],\"ts\":%lu}", r,g,b,millis());
  lastLedJson = String(buf);
  mqtt.publish(TOPIC_LED, buf, true);
  lastMsgMillis = millis();
  DBG("[MQTT] led -> %s\n", buf);
}

// -------------------- Dashboard (HTML) -----------------------
const char DASH_HTML[] PROGMEM = R"rawliteral(
<!doctype html><html lang="pt-br"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ambient Match Dashboard</title>
<style>
:root{--bg:#0f172a;--txt:#e5e7eb;--mut:#9ca3af;--panel:#0b1324;--border:#1f2937}
*{box-sizing:border-box;font-family:system-ui,-apple-system,Segoe UI,Roboto,Ubuntu,Arial}
body{margin:0;background:#0b1021;color:var(--txt);display:flex;justify-content:center}
.wrap{max-width:980px;width:100%;padding:22px 14px}
h1{font-size:22px;margin:0 0 14px}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:14px}
.card{background:linear-gradient(180deg,#111827 0,#0b1324 100%);border:1px solid var(--border);border-radius:16px;padding:14px}
h2{font-size:15px;margin:0 0 10px;color:#cbd5e1}
.val{font-weight:700;font-size:28px}
.badge{display:inline-block;padding:4px 10px;border-radius:999px;background:#0b1931;border:1px solid #1e293b;color:#a5b4fc}
.small{font-size:12px;color:#9ca3af}
.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
.swatch{width:48px;height:48px;border-radius:12px;border:1px solid var(--border)}
.input{background:#0b1a34;border:1px solid #1f2a3c;color:#e5e7eb;border-radius:10px;padding:8px 10px}
button{background:#0ea5e9;border:none;color:white;padding:8px 14px;border-radius:10px;cursor:pointer}
button:hover{filter:brightness(1.1)}
@media (max-width:720px){.grid{grid-template-columns:1fr}}
</style></head><body>
<div class="wrap">
  <h1>Ambient Match — ESP32</h1>
  <div class="grid">
    <div class="card">
      <h2>Status</h2>
      <div class="row">
        <span class="badge" id="wifi">Wi-Fi: ok</span>
        <span class="badge" id="last">Última msg: —</span>
      </div>
      <div class="small" id="hint">Atualizando da própria placa (HTTP)</div>
    </div>
    <div class="card">
      <h2>Lux (BH1750)</h2>
      <div class="val" id="lux">—</div>
      <div class="small">tópico: <code>LazaroNicolas/ambient/lux</code></div>
    </div>
    <div class="card">
      <h2>Cor detectada (TCS3200)</h2>
      <div class="row">
        <div class="swatch" id="sw"></div>
        <div>
          <div class="val" id="cname">—</div>
          <div class="small" id="cmeta">RGB — | HSV —</div>
        </div>
      </div>
      <div class="small">tópico: <code>LazaroNicolas/ambient/color</code></div>
    </div>
    <div class="card">
      <h2>LED (atuador)</h2>
      <div class="row">
        <input class="input" id="r" type="number" min="0" max="255" value="0" style="width:80px">
        <input class="input" id="g" type="number" min="0" max="255" value="0" style="width:80px">
        <input class="input" id="b" type="number" min="0" max="255" value="0" style="width:80px">
        <button id="btnSend">Enviar</button>
      </div>
      <div class="small">envio via HTTP → ESP32 (que publica no MQTT)</div>
    </div>
    <div class="card" style="grid-column:1/-1">
      <h2>Log</h2>
      <div id="log" class="small"></div>
    </div>
  </div>
</div>
<script>
function log(s){const el=document.getElementById('log');const p=document.createElement('div');
  p.textContent=new Date().toLocaleTimeString()+" - "+s; el.prepend(p);}
async function pull(){
  try{
    const r = await fetch('/api/state');
    if(!r.ok){ log('HTTP '+r.status); return; }
    const d = await r.json();
    document.getElementById('last').innerText = "Última msg: "+new Date(d.now).toLocaleTimeString();

    // Lux
    if(d.lux && typeof d.lux.lux !== 'undefined'){
      document.getElementById('lux').innerText = d.lux.lux.toFixed(2);
    }

    // Cor
    if(d.color && d.color.rgb){
      const [r,g,b] = d.color.rgb;
      const hex="#"+[r,g,b].map(x=>('0'+x.toString(16)).slice(-2)).join('');
      document.getElementById('sw').style.background = hex;
      document.getElementById('cname').innerText = d.color.color || "—";
      if(d.color.hsv){
        const h=d.color.hsv.h||0, s=d.color.hsv.s||0, v=d.color.hsv.v||0;
        document.getElementById('cmeta').innerText = `RGB (${r},${g},${b}) | HSV (${h.toFixed(0)},${s.toFixed(2)},${v.toFixed(2)})`;
      } else {
        document.getElementById('cmeta').innerText = `RGB (${r},${g},${b}) | HSV —`;
      }
    }
  }catch(e){ log('erro: '+e); }
}
setInterval(pull, 700); // ~1.4 Hz
pull();

document.getElementById('btnSend').onclick = async ()=>{
  const r=+document.getElementById('r').value||0;
  const g=+document.getElementById('g').value||0;
  const b=+document.getElementById('b').value||0;
  try{
    const res = await fetch('/api/cmd', {
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body: JSON.stringify({led:[r,g,b]})
    });
    log('cmd HTTP -> {"led":['+r+','+g+','+b+']} ('+res.status+')');
  }catch(e){ log('falha cmd: '+e); }
};
</script>
</body></html>
)rawliteral";

// -------------------- API HTTP -------------------------------
void handleRoot(){
  server.send(200, "text/html; charset=utf-8", DASH_HTML);
}

// GET /api/state  -> snapshot JSON com últimos valores
void handleState(){
  // monta um JSON leve juntando os últimos buffers
  String json = "{";
  json += "\"now\":" + String((unsigned long)(millis())) + ",";
  json += "\"lux\":"   + lastLuxJson + ",";
  json += "\"color\":" + lastColorJson + ",";
  json += "\"led\":"   + lastLedJson;
  json += "}";
  server.send(200, "application/json", json);
}

// POST /api/cmd  body: {"led":[r,g,b]}
void handleCmd(){
  if (server.hasArg("plain")){
    String body = server.arg("plain");
    int r,g,b;
    if (sscanf(body.c_str(), "{\"led\":[%d,%d,%d]}", &r,&g,&b) == 3){
      r = constrain(r,0,255); g = constrain(g,0,255); b = constrain(b,0,255);
      setLedRGB((uint8_t)r,(uint8_t)g,(uint8_t)b);
      curR = r; curG = g; curB = b;
      lastLedApply = millis();
      // publica no MQTT também (opcional)
      char buf[96];
      snprintf(buf,sizeof(buf), "{\"led_rgb\":[%d,%d,%d],\"ts\":%lu}", r,g,b,millis());
      lastLedJson = String(buf);
      mqtt.publish(TOPIC_LED, buf, true);
      server.send(200, "application/json", String("{\"ok\":true}"));
      return;
    }
  }
  server.send(400, "application/json", "{\"ok\":false}");
}

// ============================ SETUP ===========================
void setup(){
  Serial.begin(115200); delay(200);
  Serial.println("\n=== Ambient Match: TCS3200 + BH1750 + LED + MQTT + HTTP Dashboard ===");

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
  // inicializa estado do LED
  setLedRGB(0,0,0);
  curR = curG = curB = 0;
  lastLedApply = millis();

  // BH1750 (I2C em 26/27)
  I2Cbh.begin(SDA_BH, SCL_BH, 400000);
  bool bhOk = lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE, 0x23, &I2Cbh);
  if(!bhOk) Serial.println("[BH1750] ERRO (tente ADDR 0x5C).");
  else      Serial.println("[BH1750] OK.");

  // Wi-Fi / MQTT
  wifiConnect();
  mqtt.setServer(mqtt_server, mqtt_port);
  mqtt.setCallback(onMqttMessage);

  // WebServer (dashboard + API)
  server.on("/",         HTTP_GET,  handleRoot);
  server.on("/api/state",HTTP_GET,  handleState);
  server.on("/api/cmd",  HTTP_POST, handleCmd);
  server.begin();
  Serial.printf("[HTTP] Dashboard: http://%s/\n", WiFi.localIP().toString().c_str());
}

// ============================= LOOP ===========================
void loop(){
  if(WiFi.status()!=WL_CONNECTED) wifiConnect();
  if(!mqtt.connected()) mqttReconnect();
  mqtt.loop();
  server.handleClient();

  // ---- TCS3200: medir cor ----
  float fR = avgFreqForFilter(LOW,LOW);     // Red
  float fB = avgFreqForFilter(LOW,HIGH);    // Blue
  float fG = avgFreqForFilter(HIGH,HIGH);   // Green

  uint8_t R = mapFreqTo8(fR,Fmin_R,Fmax_R);
  uint8_t G = mapFreqTo8(fG,Fmin_G,Fmax_G);
  uint8_t B = mapFreqTo8(fB,Fmin_B,Fmax_B);

  float H,S,V; rgbToHsv(R,G,B,H,S,V);
  const char* name = classifyColor(H,S,V);

  // ---- BH1750: lux ----
  float lux = lightMeter.readLightLevel();

  // ---- Brilho do LED pelo lux (cor alvo calculada)
  float k = lux / 800.0f; if(k<0.12f) k=0.12f; if(k>1.0f) k=1.0f;
  uint8_t rOut = (uint8_t)(R * k);
  uint8_t gOut = (uint8_t)(G * k);
  uint8_t bOut = (uint8_t)(B * k);

  // ===== Troca do LED com hold de 1s (não bloqueante) =====
  bool wouldChange =
    (abs((int)rOut - (int)curR) >= RGB_DELTA) ||
    (abs((int)gOut - (int)curG) >= RGB_DELTA) ||
    (abs((int)bOut - (int)curB) >= RGB_DELTA);

  unsigned long now = millis();
  if (wouldChange && (now - lastLedApply) >= LED_HOLD_MS) {
    setLedRGB(rOut, gOut, bOut);
    curR = rOut; curG = gOut; curB = bOut;
    lastLedApply = now;

    // publica o estado REAL do LED quando aplicarmos a troca
    publishLed(curR, curG, curB);
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
    // LED: publica no heartbeat o estado REAL (evita duplicar com o publish no apply)
    if(heartbeat) publishLed(curR, curG, curB);

    lastR = R; lastG = G; lastB = B; lastLux = lux; lastPublish = now;
  }

  delay(250); // loop ~4 Hz (sem travar nada)
}
