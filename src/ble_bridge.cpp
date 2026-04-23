#include "ble_bridge.h"
#include <NimBLEDevice.h>
#include <Arduino.h>
#include <esp_system.h>
#include <string.h>

#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

static const size_t RX_CAP = 2048;
static uint8_t  rxBuf[RX_CAP];
static volatile size_t rxHead = 0;
static volatile size_t rxTail = 0;

static NimBLEServer*         server    = nullptr;
static NimBLECharacteristic* txChar    = nullptr;
static NimBLECharacteristic* rxChar    = nullptr;
static volatile bool         connected = false;
static volatile bool         secure    = false;
static volatile uint32_t     passkey   = 0;
static volatile uint16_t     mtu       = 23;

static void rxPush(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    size_t next = (rxHead + 1) % RX_CAP;
    if (next == rxTail) return;
    rxBuf[rxHead] = p[i];
    rxHead = next;
  }
}

static void rxClear() {
  memset(rxBuf, 0, sizeof(rxBuf));
  rxHead = 0;
  rxTail = 0;
}

class RxCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    if (!v.empty()) rxPush((const uint8_t*)v.data(), v.size());
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer*) override {
    connected = true;
    Serial.println("[ble] connected");
  }
  void onDisconnect(NimBLEServer*) override {
    connected = false;
    secure    = false;
    passkey   = 0;
    mtu       = 23;
    rxClear();
    Serial.println("[ble] disconnected");
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t newMtu, ble_gap_conn_desc*) override {
    mtu = newMtu;
    Serial.printf("[ble] mtu=%u\n", newMtu);
  }
  uint32_t onPassKeyRequest() override {
    if (passkey == 0) {
      passkey = 100000 + (esp_random() % 900000);
      Serial.printf("[ble] passkey %06lu\n", (unsigned long)passkey);
    }
    return passkey;
  }
  bool onConfirmPIN(uint32_t) override { return false; }
  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    passkey = 0;
    secure  = desc->sec_state.encrypted && desc->sec_state.authenticated;
    Serial.printf("[ble] auth %s\n", secure ? "ok" : "FAIL");
    if (!secure && server) server->disconnect(desc->conn_handle);
  }
};

void bleInit(const char* deviceName) {
  NimBLEDevice::init(deviceName);
  NimBLEDevice::setMTU(517);

  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, MITM, SC
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

  server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService* svc = server->createService(NUS_SERVICE_UUID);

  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ_ENC
  );

  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC
  );
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  NimBLEDevice::startAdvertising();
  Serial.printf("[ble] advertising as '%s'\n", deviceName);
}

bool bleConnected() { return connected; }
bool bleSecure()    { return secure; }
uint32_t blePasskey() { return passkey; }

void bleClearBonds() {
  NimBLEDevice::deleteAllBonds();
  Serial.println("[ble] bonds cleared");
}

size_t bleAvailable() {
  return (rxHead + RX_CAP - rxTail) % RX_CAP;
}

int bleRead() {
  if (rxHead == rxTail) return -1;
  int b = rxBuf[rxTail];
  rxTail = (rxTail + 1) % RX_CAP;
  return b;
}

size_t bleWrite(const uint8_t* data, size_t len) {
  if (!connected || !txChar) return 0;
  size_t chunk = mtu > 3 ? mtu - 3 : 20;
  if (chunk > 180) chunk = 180;
  size_t sent = 0;
  while (sent < len) {
    size_t n = len - sent;
    if (n > chunk) n = chunk;
    txChar->setValue((uint8_t*)(data + sent), n);
    txChar->notify();
    sent += n;
    delay(4);
  }
  return sent;
}
