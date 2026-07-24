# Projekt BLE Mesh - Node Friend (provisioning na sztywno)

Wariant projektu, w ktorym **nie ma provisionera** - kazdy wezel ma
provisioning wpisany na sztywno w firmware (stale klucze i staly adres) i
zaraz po starcie sam wchodzi do sieci BLE Mesh, bez handshake'u PB-ADV i bez
zewnetrznej konfiguracji. Klucze (NetKey/AppKey) sa identyczne we wszystkich
wezlach i sluza tez do sniffowania w Wiresharku.

## Aplikacje (`app/apps/`)

- **friend** - Node Friend na nRF52840 Dongle. Rola Friend + Sensor Client
  (odbiera i loguje temperature publikowana przez LPN). Adres `0x0001`.
- **lpn** - Low Power Node na BTZ_EndDevice (nRF54L15). Sensor Server z realnym
  czujnikiem STS4X, publikuje temperature do Frienda. Adres `0x0002`.
- **lpn_mock** - jak `lpn`, ale bez fizycznego czujnika (zmyslona temperatura
  22.5 C). Sluzy do bisekcji problemow. Adres `0x0002`.

> `lpn` i `lpn_mock` NIGDY nie dzialaja w sieci jednoczesnie (uruchamiane
> osobno), dlatego wspoldziela ten sam adres unicast `0x0002`.

## Jak to dziala (hardcoded provisioning)

Kazdy wezel w `main.c`:

1. `bt_mesh_init()` + `bt_mesh_provision()` ze stalym NetKey/DevKey i stalym
   adresem - wezel jest od razu w sieci.
2. Lokalna konfiguracja przez loopback Config Client do wlasnego adresu:
   dodanie AppKey, bind modeli, (LPN) ustawienie publikacji Sensor Servera na
   Frienda (`0x0001`).
3. Friend: `bt_mesh_friend_set(ENABLED)`. LPN: `bt_mesh_lpn_set(true)`.

Wszystkie wezly sa bezstanowe (`CONFIG_BT_SETTINGS=n`) - po restarcie
odtwarzaja dokladnie ten sam stan z firmware.

## Budowanie

```
west build -b nrf52840dongle/nrf52840     -p auto -d build/friend   app/apps/friend
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p auto -d build/lpn      app/apps/lpn
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p auto -d build/lpn_mock app/apps/lpn_mock
```

Gotowe zadania sa tez w `.vscode/tasks.json`.
