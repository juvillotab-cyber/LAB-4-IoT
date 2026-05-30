# Lab 4: Reliable Downlink & Actuator Control (CoAP CON)

## SoilSense Project — Phase: Control Logic

---

## Resumen de la sesión — problemas y soluciones

### Task A — Round-trip `/act/valve`

**Objetivo:** Enviar PUT CoAP desde Node B para abrir/cerrar la válvula en Node V, con respuesta `2.04 Changed` y LED.

**Problema 1: El comando `tlv 60` no funciona en el OpenThread CLI.**
- El SOP dice `coap put ... con tlv 60 a1617601`
- `tlv` se interpreta como payload literal (3 bytes "tlv"), no como content-format
- El CLI ignora `60` y `a1617601`
- Resultado: `malformed CBOR (3 B)` en Node V

**Solución:** El comando correcto es sin `tlv 60`:
```
> ot coap put <addr> act/valve con a1617601
```

**Problema 2: El OT CLI envía payload como ASCII, no como binario.**
- `a1617601` se envía como 8 caracteres ASCII, no como 4 bytes CBOR
- El server esperaba exactamente 4 bytes CBOR y rechazaba cualquier otra longitud

**Solución (código):** Se modificó `decode_valve_cbor()` en `valve_demo.c` para que detecte ASCII hex (8 caracteres), lo convierta a binario (`A1 61 76 01`), y luego lo decodifique como CBOR. Así funciona tanto con el OT CLI (ASCII hex) como con clientes CoAP reales (binario directo).

**Archivos creados/modificados en la sesión:**

| Archivo | Cambio |
|---|---|
| `main/valve_demo.c` | **Nuevo.** Servidor CoAP `/act/valve`. Se modificó `decode_valve_cbor()` respecto al SOP-04. **Original** (SOP-04): solo aceptaba 4 bytes binarios CBOR. **Modificación:** se agregó `char2nibble()` y `decode_valve_cbor()` ahora acepta 4 bytes binarios directo **o** 8 bytes ASCII hex → los convierte a binario → y los decodifica como CBOR. |

**Código original (SOP-04) — solo 4 bytes CBOR:**
```c
static bool decode_valve_cbor(const uint8_t *buf, size_t len, uint8_t *out_v)
{
    if (len != 4)                                     return false;
    if (buf[0] != 0xA1)                               return false;
    if (buf[1] != 0x61 || buf[2] != 0x76)             return false;
    if (buf[3] != 0x00 && buf[3] != 0x01)             return false;
    *out_v = buf[3];
    return true;
}
```

**Código modificado — acepta CBOR binario (4B) o ASCII hex (8B):**
```c
static bool char2nibble(char c, uint8_t *n)
{
    if (c >= '0' && c <= '9') { *n = c - '0'; return true; }
    if (c >= 'a' && c <= 'f') { *n = c - 'a' + 10; return true; }
    if (c >= 'A' && c <= 'F') { *n = c - 'A' + 10; return true; }
    return false;
}

static bool decode_valve_cbor(const uint8_t *buf, size_t len, uint8_t *out_v)
{
    if (len == 4) {
        if (buf[0] != 0xA1)                               return false;
        if (buf[1] != 0x61 || buf[2] != 0x76)             return false;
        if (buf[3] != 0x00 && buf[3] != 0x01)             return false;
        *out_v = buf[3];
        return true;
    }
    if (len == 8) {
        uint8_t raw[4];
        for (int i = 0; i < 4; i++) {
            uint8_t hi, lo;
            if (!char2nibble((char)buf[2*i], &hi)) return false;
            if (!char2nibble((char)buf[2*i+1], &lo)) return false;
            raw[i] = (hi << 4) | lo;
        }
        return decode_valve_cbor(raw, 4, out_v);
    }
    return false;
}
```
| `main/CMakeLists.txt` | Agregado `valve_demo.c` a SRCS y dependencia `driver` a REQUIRES (necesaria para `driver/gpio.h`). |
| `main/esp_ot_cli.c` | Agregado forward declaration `start_valve_server()`. Arranque condicional: si `CONFIG_OPENTHREAD_MTD` → `start_valve_server()`, si no → `start_coap_server()`. Agregado `esp_log_level_set("coap_demo", ESP_LOG_NONE)` para silenciar logs del sensor en Node A/B. |
| `main/esp_ot_custom_config.h` | Agregado `OPENTHREAD_CONFIG_MAC_DEFAULT_DATA_POLL_PERIOD 5000`. |
| `sdkconfig.defaults` | Agregado `CONFIG_OPENTHREAD_FTD=y` explícito. |
| `sdkconfig.defaults.sed` | **Nuevo.** Defaults para Node V: MTD, RX_ON_WHEN_IDLE desactivado, PM_ENABLE. |
| `README.md` | Este documento. |

**Comandos finales:**
```
> ot get fd39:... act/valve                          # leer estado
> ot coap put fd39:... act/valve con a1617601        # abrir ({"v":1})
> ot coap put fd39:... act/valve con a1617600        # cerrar ({"v":0})
```

---

### Task B — CON reliability under loss

**Objetivo:** Demostrar que CoAP CON retransmite cuando el destino no responde, con backoff exponencial (~2s → ~4s → ~8s → ~16s → timeout ~45s), y que después de reconectar el PUT funciona y el ACK llega.

**Procedimiento:**
1. Desconectar Node V: `ot thread stop`
2. En Node B: `ot coap put ... con a1617601`
3. Observar retransmisiones (drops en MeshForwarder)
4. Reconectar Node V: `ot thread start`
5. Re-enviar el PUT → `coap response 2.04 Changed`

**Problema: Capturar logs de retransmisiones.**
- `screen /dev/ttyACM0 115200` + `Ctrl+A H` para loggear
- Filtrar con `grep -a "Dropping\|5683" screenlog.0`
- Identificar 3-4 drops con timestamps crecientes

**Resultado de retransmisiones capturadas:**
| Drop | Timestamp | Intervalo | error |
|------|-----------|-----------|-------|
| 1º | 666,875 | — | AddressQuery |
| 2º | 668,515 | ~1.6 s | Drop |
| 3º | 673,275 | ~4.8 s | Drop |

**Problema: Ruido excesivo en logs con `ot log level 4`.**
- `MeshForwarder` y `Mle` imprimen constantemente (Advertisements, polls, etc.)
- `ot log level 2` silencia todo pero también oculta los drops
- `ot log level 3` no muestra INFO (los drops son INFO)

**Solución:** Usar `ot log level 4` y filtrar con `grep`:
```bash
grep -a -E "Dropping|5683" screenlog.0
```

**Lección:** CoAP CON retransmite en la capa de mensajes (mismo MID). El server usa dedup cache para evitar procesar duplicados. PUT es idempotente → aplicar `{v:1}` dos veces da el mismo estado.

---

### Task C — SED poll period vs latency

**Objetivo:** Medir la latencia de downlink con diferentes poll periods (1s, 5s, 30s) y calcular el impacto en batería.

**Problema 1: Node V no entraba en sleep (respondía instantáneo siempre).**
Aunque se configuró menuconfig con MTD + RX_ON_WHEN_IDLE desactivado, el modo Thread guardado en NVS mantenía `r` (rx_on_when_idle) desde configuraciones anteriores.

**Solución:**
```
> ot mode -          # remover flag 'r'
> ot thread stop
> ot thread start
```
Verificar:
```
> ot mode            # debe estar VACÍO (sin r)
> ot state           # child
```
Además, en menuconfig se requieren **4 configuraciones** que faltaban:
1. `OpenThread → Thread device type → Minimal Thread Device (MTD)`
2. `OpenThread → Enable OpenThread radio capability rx on when idle` → **DESMARCAR**
3. `Power Management → Support for power management` → **MARCAR**
4. `FreeRTOS → Kernel → Enable tickless idle support` → **MARCAR** (sin esto `esp_openthread_sleep.c` no compila)

**Problema 2: `ot mode` mostraba `r` incluso después de `ot factoryreset`.**
El modo persiste en NVS. Solo `ot mode -` lo remueve efectivamente.

**Problema 3: Medir latencia sin cronómetro.**
El `coap response` del OT CLI no tiene timestamp. Soluciones:
- Usar timestamps del log `I(XXXXXX)` en pares `dst:` y `src:`
- Usar cronómetro de celular (presionar Enter y medir hasta ver `coap response`)
- `ot log level 4` + grep de `e03e\]:5683`

**Mediciones obtenidas:**
| poll_period | Latencia peor caso | Cálculo energía |
|---|---|---|
| 1 s | ~1 s | (10ms/1000ms)×75mA + 5µA ≈ 755µA → ~276 días |
| 5 s | ~5 s | (10ms/5000ms)×75mA + 5µA ≈ 155µA → ~3.7 años |
| 30 s | ~30 s | (10ms/30000ms)×75mA + 5µA ≈ 30µA → ~19 años |
**Evidencia en video:**
Pollperiod.webm

**Poll period elegido (ADR-004):** 5 s — balance entre latencia aceptable (~5s, imperceptible para un humano) y batería de meses.

**Verificación de SED funcionando:** en el log de arranque debe aparecer:
```
I (766) esp openthread sleep: Enable OpenThread light sleep, the wake up source is ESP timer
```
Y en el Router padre deben verse `DataPollHandlr: Rx data poll, src:0xd406` cada ~1s.

---

## Errores comunes y soluciones rápidas

| Error | Causa | Solución |
|---|---|---|
| `malformed CBOR (3 B)` | Usaste `tlv 60` | Quitar `tlv 60`, enviar hex directo |
| `malformed CBOR (8 B)` | Payload en ASCII en vez de binario | El server acepta ASCII hex (8 chars) |
| `4.00 Bad Request` | Payload mal formado | Usar `a1617601` o `a1617600` |
| Node V responde instantáneo | `r` activado en modo | `ot mode -` + `thread stop/start` |
| `pollperiod` no cambia latencia | RX_ON_WHEN_IDLE activado | Desmarcar en menuconfig + reflash |
| No aparecen drops en log | `log level` muy bajo | Usar `ot log level 4` |
| `Enable OpenThread light sleep` no aparece | Falta `configUSE_TICKLESS_IDLE` | Marcarlo en FreeRTOS Kernel |
| Node V está como `router` no `child` | MTD no configurado | Configurar MTD + reflash |

---

## Comandos útiles

```bash
# Capturar logs
screen /dev/ttyACM0 115200
Ctrl+A H         # activar log (screenlog.0)
Ctrl+A H         # desactivar log
Ctrl+A k y       # salir de screen

# Filtrar logs
grep -a "e03e\]:5683" screenlog.0       # tráfico a Node V
grep -a "coap response" screenlog.0     # respuestas CoAP
grep -a "Dropping" screenlog.0          # retransmisiones

# SED - verificar modo
ot mode           # sin 'r' = SED
ot pollperiod     # periodo actual
ot state          # debe ser 'child'
ot log level 4    # ver retransmisiones
```

## Compilación rápida

```bash
# Node V (MTD/SED)
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.sed" menuconfig
idf.py build
idf.py -p /dev/ttyACM0 erase-flash flash monitor
```
