# Projekt BLE Mesh - Node Friend (Friend jako provisioner)

Wariant projektu, w ktorym **Friend pelni role provisionera**. Friend tworzy
siec, sam do niej wchodzi, a nastepnie realnie provisionuje i konfiguruje LPN
przez powietrze (PB-ADV) - bez OOB. Klucze (NetKey/AppKey) sa stale i wspolne
dla sieci; sluza tez do sniffowania w Wiresharku, ale teraz sa **rozdystrybuowane
przez provisioning**, a nie wpisane na sztywno w LPN.

## Aplikacje (`app/apps/`)

- **friend** - Node Friend na nRF52840 Dongle. Rola Friend + Provisioner +
  Sensor Client (odbiera i loguje temperature publikowana przez LPN). Tworzy
  siec (CDB), self-provisionuje sie na `0x0001`, provisionuje LPN na `0x0002`
  i zdalnie go konfiguruje (Config Client).
- **lpn** - Low Power Node na BTZ_EndDevice (nRF54L15). Sensor Server z realnym
  czujnikiem STS4X. Wystawia unprovisioned beacon i czeka na provisioning od
  Frienda. Adres `0x0002` (przydzielany przez Frienda).
- **lpn_mock** - jak `lpn`, ale bez fizycznego czujnika (zmyslona temperatura
  22.5 C) i **nadal z provisioningiem na sztywno** (self-provision tymi samymi
  stalymi kluczami). Sluzy do bisekcji problemow - dolacza do sieci Frienda
  bezposrednio, bez PB-ADV. Adres `0x0002`.

> `lpn` i `lpn_mock` NIGDY nie dzialaja w sieci jednoczesnie (uruchamiane
> osobno), dlatego wspoldziela ten sam adres unicast `0x0002`.

## Jak to dziala (Friend = provisioner)

**Friend** (`apps/friend/main.c`):

1. `bt_mesh_init()` + `bt_mesh_cdb_create(net_key)` - tworzy siec w bazie
   provisionera (CDB, w RAM).
2. `bt_mesh_provision()` - Friend sam wchodzi do wlasnej sieci na `0x0001`,
   loopback Config Client binduje AppKey do wlasnego Sensor Client.
3. `bt_mesh_friend_set(ENABLED)`.
4. Callback `unprovisioned_beacon` - reaguje **tylko** na znany UUID LPN. Jesli
   LPN byl juz w CDB (restart), usuwa stary wpis i czysci RPL, po czym
   `bt_mesh_provision_adv()` provisionuje LPN na staly adres `0x0002`.
5. Callback `node_added` - Friend zdalnie konfiguruje LPN przez Config Client:
   AppKey add, bind Sensor/Health Server, publikacja Sensor Servera na `0x0001`.

**LPN** (`apps/lpn/main.c`):

1. `bt_mesh_init()` + `bt_mesh_prov_enable(BT_MESH_PROV_ADV)` - wystawia
   unprovisioned beacon i czeka.
2. Po provisioningu i zdalnej konfiguracji stack sam wchodzi w tryb LPN
   (`CONFIG_BT_MESH_LPN_AUTO`, po ~15 s ciszy - zwloka daje Friendowi czas na
   konfiguracje) i szuka Frienda. Publikacja pomiarow rusza po nawiazaniu
   friendship.

## Trwalosc i restarty

Wszystkie wezly sa bezstanowe (`CONFIG_BT_SETTINGS=n`) - nic nie zapisuja do
flasha. Po restarcie Friend odtwarza siec i CDB od zera. Gdy zrestartuje sie
**LPN**, wystawia beacon ponownie; Friend (ciagle skanuje) wykrywa znany UUID,
usuwa stary wpis z CDB i provisionuje go od nowa na `0x0002` - bez ingerencji
uzytkownika.

## Budowanie

```
west build -b nrf52840dongle/nrf52840     -p auto -d build/friend   app/apps/friend
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p auto -d build/lpn      app/apps/lpn
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p auto -d build/lpn_mock app/apps/lpn_mock
```

Gotowe zadania sa tez w `.vscode/tasks.json`.
