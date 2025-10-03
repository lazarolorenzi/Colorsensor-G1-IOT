# 😎 Ambient Match — IoT com ESP32, TCS3200, BH1750 e LED RGB

Este projeto foi desenvolvido para a disciplina de IoT.
A ideia foi criar um **sistema embarcado inteligente** capaz de capturar informações do ambiente (cor e luminosidade), processar os dados e **reproduzir a cor detectada em um LED RGB**, além de publicar os valores em **MQTT** e disponibilizar uma **dashboard local via HTTP**.

A motivação é explorar a integração de múltiplos sensores, comunicação via MQTT, uso de atuadores e visualização em tempo real.

---

## 🎯 Objetivos do Sistema
- Integrar **múltiplos sensores (TCS3200 e BH1750)** em um mesmo projeto.  
- Reproduzir a cor detectada em um **LED RGB**, ajustando brilho conforme a luminosidade.  
- Publicar os dados em **tópicos MQTT distintos**, com suporte a comandos de controle.  
- Criar uma **dashboard local (HTTP)** para visualização em tempo real e envio de comandos.  
- Garantir **fluidez** no LED (segura cada cor por pelo menos 1 segundo antes da troca).  
- Documentar o fluxo completo do projeto, desde os sensores até a dashboard.  

---

## 🧩 Sensores Escolhidos e Justificativa

### 🔹 TCS3200 — Sensor de Cor
- Capaz de detectar a composição RGB de superfícies.
- Justificativa: Permite criar sistemas de reconhecimento de cor com baixo custo e fácil integração.

### 🔹 BH1750 — Sensor de Luminosidade
- Mede intensidade de luz em lux.
- Justificativa: Complementa a percepção ambiental, ajustando o brilho do LED de acordo com a iluminação do local.

### 🔹 LED RGB (4 pinos)
- Ator principal do sistema, exibindo a cor detectada.
- Justificativa: Feedback visual imediato, ideal para testes em tempo real.

---

## 🖥️ Estrutura do Código

O código está organizado em módulos lógicos:  
- **Leitura de sensores** (TCS3200 e BH1750).  
- **Processamento** (conversão RGB/HSV e classificação de cor).  
- **Atuação** (LED RGB com hold de 1 segundo por cor).  
- **Comunicação** (Wi-Fi, MQTT e servidor HTTP).  

### Trechos principais comentados:

```cpp
// ======= Controle de troca do LED (hold de 1s) =======
uint8_t curR = 0, curG = 0, curB = 0;           // última cor aplicada
unsigned long lastLedApply = 0;                  // instante da última troca
const unsigned long LED_HOLD_MS = 1000;          // segurar 1 segundo

// Se a cor mudou e já passou 1s desde a última troca, aplica a nova cor
if (wouldChange && (now - lastLedApply) >= LED_HOLD_MS) {
    setLedRGB(rOut, gOut, bOut);
    curR = rOut; curG = gOut; curB = bOut;
    lastLedApply = now;
    publishLed(curR, curG, curB); // publica o estado real
}
```

---

## 📡 Fluxo MQTT

O ESP32 publica mensagens em tópicos distintos no broker **test.mosquitto.org**:

- `LazaroNicolas/ambient/lux` → valores de luminosidade (lux).  
- `LazaroNicolas/ambient/color` → dados de cor (RGB + HSV + classificação).  
- `LazaroNicolas/ambient/led` → estado atual do LED RGB.  
- `LazaroNicolas/ambient/status` → status online/offline do dispositivo.  
- `LazaroNicolas/ambient/cmd` → canal de **comandos externos** para mudar a cor manualmente.  

**QoS:** como estamos usando `mqtt.publish(..., true)`, o sistema mantém a última mensagem nos tópicos (retained), garantindo que clientes que entrem depois recebam os valores mais recentes.  

---

## ⚙️ Fluxo do Sistema

```mermaid
flowchart TD
    A[Sensores] --> B[ESP32]
    B --> C[Processamento RGB + Lux]
    C --> D[LED RGB (atuador)]
    C --> E[MQTT Broker]
    C --> F[Dashboard HTTP]
    E --> G[Clientes Externos]
```

---

## 📊 Dashboard

O ESP32 hospeda um **servidor HTTP** acessível no navegador.  
Funcionalidades:
- Exibe valores de lux em tempo real.  
- Mostra a cor detectada (nome + preview RGB/HSV).  
- Permite **enviar comandos manuais** para o LED.  
- Mostra log com horário das últimas atualizações.  

📸 **Capturas de Tela (instruções):**
- Coloque prints no diretório `/docs` e linke aqui:
  - Dashboard em execução → `![Dashboard](docs/dashboard.png)`  
  - LED exibindo cor → `![LED RGB](docs/led.png)`  
  - Assinatura MQTT → `![MQTT Logs](docs/mqtt.png)`  

---

## 🚀 Considerações Finais

### Desafios
- Calibrar o **TCS3200** (ajustar `Fmin` e `Fmax` para leitura precisa).  
- Garantir que o LED não piscasse rapidamente (solução: segurar cor por 1 segundo).  
- Conciliar Wi-Fi, MQTT e WebServer rodando simultaneamente no ESP32.  

### Melhorias Futuras
- Armazenar dados em um banco externo (via Flask + SQLite/Postgres).  
- Criar gráficos históricos (Grafana/Node-RED).  
- Adicionar novos sensores (ex.: DHT22 para temperatura/umidade).  
- Suporte a MQTT com TLS para maior segurança.  

---

✍️ Desenvolvido por **Lázaro Vissoto Lorenzi**  
Projeto acadêmico — Faculdade Antonio Meneghetti
