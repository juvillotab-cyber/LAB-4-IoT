# Lab 4: Reliable Downlink & Actuator Control (CoAP CON)

## SoilSense Project — Phase: Control Logic

CoAP/CBOR downlink sobre Thread a un Sleepy End Device (SED).

## Requisitos

- ESP-IDF v5.1+
- 3 placas ESP32-C6
- IEEE 802.15.4 (Thread)

## Compilación

### Node A (FTD, sensor `/env/temp`)
```bash
idf.py set-target esp32c6
idf.py menuconfig   # Debe ser FTD por defecto
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

### Node B (FTD, CLI cliente)
```bash
idf.py -p /dev/ttyUSB1 flash monitor
```

### Node V (MTD/SED, válvula `/act/valve`)
```bash
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.sed" menuconfig
idf.py build
idf.py -p /dev/ttyUSB2 flash monitor
```

## Recursos CoAP

| Recurso | Métodos | Payload | Descripción |
|---------|---------|---------|-------------|
| `/env/temp` | GET (Observe) | `{"t": float16}` | Temperatura simulada (Lab 3) |
| `/act/valve` | PUT (CON), GET | `{"v": 0\|1}` | Control de válvula (Lab 4) |

## Roles por placa

| Board | Config | Server |
|-------|--------|--------|
| Node A | FTD | `/env/temp` |
| Node B | FTD | Ninguno (CLI) |
| Node V | MTD/SED | `/act/valve` |
