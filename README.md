# Cybex E-Priam ESPHome Bridge

ESP32-basert Bluetooth-bridge for å styre Cybex E-Priam barnevogn via Home Assistant.

## Basert på

- [python-priam](https://github.com/vincegio/python-priam) av vincegio

## Funksjoner

- 🔋 **Batterinivå** - Viser batteriprosent
- 🍃 **Kjøremodus** - ECO, TOUR, BOOST
- 💤 **Vugge-funksjon** - Av, Lav, Medium, Høy

## Hardware

Du trenger:
- ESP32 DevKit (hvilken som helst variant)
- USB-kabel og strømforsyning

## Installasjon

### 1. Finn MAC-adresse

Først må du finne MAC-adressen til din E-Priam. Slå på barnevognen og kjør en BLE-scan:

```bash
# Med nRF Connect app på mobil, eller:
# ESPHome logger vil vise enheter med manufacturer_data 1933 (0x0791)
```

### 2. Oppdater konfigurasjon

Rediger `priam_bridge.yaml`:

```yaml
ble_client:
  - mac_address: "XX:XX:XX:XX:XX:XX"  # Din Priam MAC-adresse
```

### 3. Secrets

Lag `secrets.yaml` i ESPHome-mappen:

```yaml
wifi_ssid: "DittWiFi"
wifi_password: "DittPassord"
api_encryption_key: "generert-nøkkel-her"
ota_password: "ditt-ota-passord"
```

### 4. Flash ESP32

```bash
esphome run priam_bridge.yaml
```

## Bluetooth UUIDs

| Characteristic | UUID | Funksjon |
|----------------|------|----------|
| Service | `a1fc0100-78d3-40c2-9b6f-3c5f7b2797df` | Hoved-service |
| Status | `a1fc0102-...` | Batteri, status (notify) |
| Drive Mode | `a1fc0103-...` | ECO/TOUR/BOOST (write) |
| Rocking | `a1fc0104-...` | Vugge-kontroll (write) |

## Kommandoer

### Vugge (Rocking)

Bytes: `[intensity, duration_low, duration_high]`

- `intensity`: 0=Av, 1=Høy, 2=Medium, 3=Lav
- `duration`: Antall sekunder (little-endian, default 55 = 0x37)

Eksempel: `[0x02, 0x37, 0x00]` = Medium vugge i 55 sekunder

### Kjøremodus (Drive Mode)

Bytes: `[mode]`

- `0x01` = ECO
- `0x02` = TOUR
- `0x03` = BOOST (udokumentert i appen)

## Home Assistant Entities

Etter tilkobling får du:

- `binary_sensor.priam_tilkoblet` - Bluetooth-tilkobling
- `sensor.priam_batteri` - Batterinivå
- `button.priam_vugge_*` - Vugge-kontroll
- `button.priam_modus_*` - Kjøremodus

## Feilsøking

### ESP32 finner ikke Priam

1. Sjekk at barnevognen er slått på
2. Lukk Cybex-appen på mobilen (kun én BLE-tilkobling om gangen)
3. Prøv å restarte ESP32

### Tilkobling feiler

- Priam har manufacturer_data `1933` (0x0791)
- Sjekk at MAC-adressen er riktig
- BLE-rekkevidde er begrenset (~10 meter)

### Kommandoer virker ikke

- Sjekk at ESP32 er koblet til (binary_sensor)
- Se på ESPHome-logger for feil
- Prøv å restarte barnevognen

## Begrensninger

- Kun én BLE-tilkobling om gangen (ikke samtidig med appen)
- Rekkevidde ~10 meter
- ESP32 må være innenfor rekkevidde for kontinuerlig kontroll
