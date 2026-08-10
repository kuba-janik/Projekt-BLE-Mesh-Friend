# lpn_off / lpn_off_mock — pomiar System OFF (z retencją RAM i bez)

Dwie aplikacje pomiarowe, każda w dwóch wariantach. Cykl pracy: obudź się z System OFF →
wejdź do sieci mesh → opublikuj temperaturę → wróć do System OFF.

| aplikacja | czujnik |
|---|---|
| `lpn_off` | realny odczyt z STS4X (bramka `sensor_pwr` jak w `apps/lpn`) |
| `lpn_off_mock` | stała, sztuczna wartość 22,5 °C (bez I²C i bez STS4X) |

Warianty sterowane jednym symbolem — `CONFIG_RETAINED_MEM_NRF_RAM_CTRL`:

| wariant | co przeżywa System OFF | ścieżka startu |
|---|---|---|
| bez retencji (`prj.conf`) | nic | **zimny**: pełny provisioning PB-ADV + konfiguracja od Frienda |
| z retencją (`overlay-ret.conf`) | sekcja 32 kB RAM | **ciepły**: `bt_mesh_provision()` z retencji, zero radia na wejściu do sieci |

Mapa pamięci jest **identyczna** w obu wariantach (region retencyjny jest zawsze wykrojony),
więc różnica w pomiarze to wyłącznie efekt retencji.

## Budowanie

```bash
# lpn_off — bez retencji / z retencją
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p always -d build/lpn_off          app/apps/lpn_off
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p always -d build/lpn_off_ret      app/apps/lpn_off \
      -- -DEXTRA_CONF_FILE=overlay-ret.conf

# lpn_off_mock — bez retencji / z retencją
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p always -d build/lpn_off_mock     app/apps/lpn_off_mock
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p always -d build/lpn_off_mock_ret app/apps/lpn_off_mock \
      -- -DEXTRA_CONF_FILE=overlay-ret.conf
```

Bring-up z logami na RTT (**nie** do pomiarów prądu — logi same pobierają prąd):

```bash
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p always -d build/lpn_off_dbg app/apps/lpn_off \
      -- "-DEXTRA_CONF_FILE=overlay-ret.conf;overlay-debug.conf"
```

W logu widać, którą ścieżką poszedł start: `zimny start` albo `cieply start z retencji RAM`,
razem z numerem cyklu. Pierwszy start po flashowaniu jest zawsze zimny.

## Parametry (Kconfig, `-DCONFIG_...=`)

Nazwy są **wspólne z `apps/lpn` i `apps/lpn_mock`**, więc ta sama flaga interwału działa
we wszystkich czterech aplikacjach.

| symbol | domyślnie | znaczenie |
|---|---|---|
| `LPN_SENSOR_INTERVAL_S` | 10 | interwał publikacji — tu zarazem czas w System OFF |
| `LPN_FRIENDSHIP` | y | czy nawiązywać friendship przed publikacją |
| `LPN_FRIENDSHIP_TIMEOUT_S` | 20 | limit na Friend Offer |
| `LPN_PROV_TIMEOUT_S` | 60 | limit na provisioning (tylko zimny start) |
| `LPN_DRAIN_MS` | 500 | zwłoka po publikacji, żeby reklama wyszła w eter |
| `LPN_SEQ_MARGIN` | 32 | zapas doliczany do odtworzonego `seq` |

Tylko `LPN_SENSOR_INTERVAL_S` istnieje też w `apps/lpn`/`apps/lpn_mock` — pozostałe pięć
dotyczy wyłącznie cyklu System OFF.

`LPN_FRIENDSHIP=n` jest najciekawszym pokrętłem: izoluje koszt Friend Requesta
(okno na oferty to `FRIEND_REQ_WAIT` 100 ms + `FRIEND_REQ_SCAN` 1000 ms **ciągłego RX**).
Publikacja to czysty TX i działa bez friendship.

## Jak to działa

**Pobudka.** `z_nrf_grtc_wakeup_prepare()` uzbraja kanał GRTC, potem `sys_poweroff()`.
Działa, bo GRTC jest taktowany z LFXO (`CONFIG_NRF_GRTC_TIMER_SOURCE_LFXO=y`), więc odlicza
także w System OFF. Wyjście z OFF to **reset** — `hwinfo` raportuje wtedy `RESET_CLOCK`.

**Retencja.** `z_sys_poweroff()` (`soc/nordic/common/poweroff.c`) najpierw wyłącza retencję
dla całego RAM-u, a potem — tylko jeśli `CONFIG_RETAINED_MEM_NRF_RAM_CTRL=y` — przywraca ją
dla regionów z devicetree. Region jest w `boards/BTZ_EndDevice_nrf54l15_cpuapp.overlay`.

Granulacja retencji na nRF54L15 to **32 kB** (`RAM_SECTION_UNIT_SIZE` w nrfx), więc region
mniejszy niż 32 kB i tak podtrzymałby całą sekcję. Dlatego region ma pełne 32 kB — koszt jest
wtedy jawny, a nie ukryty.

**Co niesie retencja.** Tylko `struct retained_data` (`src/retained.h`): `iv_index`, `seq`,
adres unicast, flaga „sprovisionowany" i CRC32. Klucze są stałymi w `main.c` (te same, co
w `apps/friend` — projekt i tak trzyma je na stałe pod Wiresharka), więc jedyną rzeczą,
która **musi** przeżyć w RAM-ie, jest licznik `seq`: bez monotonicznego `seq` Friend odrzuci
publikacje na replay protection. Po restarcie `seq` jest odtwarzany przez `bt_mesh.seq`
(pole `struct bt_mesh_net`, `deps/zephyr/subsys/bluetooth/mesh/net.h`) z zapasem
`LPN_SEQ_MARGIN`.

**Czego retencja NIE niesie.** Wątków, stosów, stanu kontrolera BLE ani friendshipu.
Zephyr nie ma API do odtworzenia friendshipu LPN, więc **każda pobudka wymaga nowego
Friend Requesta** — i to jest główny koszt energetyczny tego podejścia.

## Ograniczenia, o których trzeba wiedzieć przy interpretacji wyników

- Ciepły start nie wysyła unprovisioned beacona, więc Friend nie uruchamia Config Clienta.
  Konfigurację (AppKey, bind, adres publikacji) odtwarza lokalnie
  `model_handler_restore_config()` — odpowiednik `mod_app_bind` + `mod_pub_set`.
- DevKey po ciepłym starcie jest stały i **inny** niż ten wygenerowany przez Frienda podczas
  PB-ADV. Nie ma to znaczenia, bo po ciepłym starcie nie leci żaden ruch Config; publikacje
  szyfrowane są AppKeyem, który jest zgodny.
- Health Server nie jest bindowany po ciepłym starcie — `health_pub` ma okres 0, więc nigdy
  nie publikuje.
- Gdy uzbrojenie pobudki GRTC się nie uda, węzeł robi `k_sleep()` + `sys_reboot()` zamiast
  System OFF. Cykl trwa dalej, ale pomiar z tego cyklu jest nieważny — widać to w logu.
- Każdy błąd wejścia do sieci też kończy się zaśnięciem, żeby nieobsługiwany pomiar nie zawisł.
