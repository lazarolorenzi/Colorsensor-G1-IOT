# 😎 Ambient Match — IoT com ESP32, TCS3200, BH1750 e LED RGB

## 📌 Introdução
Este projeto foi desenvolvido para a disciplina de IoT.  
A ideia foi criar um **sistema embarcado inteligente** capaz de capturar informações do ambiente (cor e luminosidade), processar os dados e **reproduzir a cor detectada em um LED RGB**, além de publicar os valores em **MQTT** e disponibilizar uma **dashboard local via HTTP**.

A motivação é explorar a integração de múltiplos sensores, comunicação via MQTT, uso de atuadores e visualização em tempo real.

---

## 🎯 Objetivos
- Integrar pelo menos **dois sensores diferentes** trabalhando juntos.  
- Implementar um **atuador (LED RGB)** que responda às leituras dos sensores.  
- Publicar dados em **tópicos MQTT distintos**.  
- Criar uma **aplicação cliente** (dashboard via navegador) que mostre os dados em tempo real.  
- Elaborar um fluxo completo de funcionamento do sistema.  

---

## 🧩 Sensores e Atuador

### 🔹 TCS3200 — Sensor de Cor
- Detecta cores usando filtros RGB.
- Retorna frequências que são convertidas em valores **RGB (0–255)**.
- As cores são classificadas em: vermelho, verde, azul, amarelo, magenta, etc.

### 🔹 BH1750 — Sensor de Luminosidade
- Mede intensidade de luz em **lux**.
- Usado para ajustar o brilho do LED em função da iluminação ambiente.

### 🔹 LED RGB (4 pinos — catodo comum)
- Recebe os valores processados e reproduz a cor detectada.
- Intensidade ajustada proporcionalmente ao nível de **lux**.

---

## 🔗 Conectividade
- **Wi-Fi**: O ESP32 conecta à rede configurada.  
- **MQTT**: Publica em `test.mosquitto.org` nos seguintes tópicos:
  - `…/ambient/lux` → medições de luminosidade.
  - `…/ambient/color` → medições do sensor de cor (RGB + HSV).
  - `…/ambient/led` → estado real do LED.
  - `…/ambient/status` → status do dispositivo.  
- **HTTP Dashboard**: Servidor embutido no ESP32 com:
  - Exibição de lux, cor e LED.
  - Preview visual da cor.
  - Log de atualizações.
  - Envio de comandos HTTP → controle manual do LED.

---

## ⚙️ Fluxo do Sistema

```mermaid
flowchart TD
    A[Sensores] --> B[ESP32]
    B --> C[Processamento RGB + Lux]
    C --> D[LED RGB]
    C --> E[MQTT Broker]
    C --> F[Dashboard HTTP]
    E --> G[Clientes Externos]
