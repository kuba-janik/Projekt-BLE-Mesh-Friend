# Projekt BLE Mesh - Friend jako Provisioner

## Przegląd

Ten projekt buduje sieć BLE Mesh z dwiema rolami: **Friend** i **LPN**
(Low Power Node). Friend działa jako provisioner. Friend dodaje LPN do
sieci samodzielnie.

- **Friend** - węzeł mesh, który cały czas nasłuchuje radia. Friend buforuje
  wiadomości dla śpiących węzłów.
- **LPN (Low Power Node)** - węzeł mesh, który śpi większość czasu. LPN
  potrzebuje Frienda do odbierania wiadomości.
- **Friendship** - relacja między Friendem i LPN-em. LPN nawiązuje friendship
  po provisioningu.
- **Provisioning** - proces dodawania nowego węzła do sieci mesh. Provisioning
przydziela węzłowi adres i klucze sieciowe.

Klucze sieciowe (NetKey, AppKey) są stałe i wspólne dla całej sieci, umożliwia to podsłuch w Wireshark.

## Architektura

### Friend

Friend wykonuje te kroki po starcie:

1. Friend tworzy sieć mesh i bazę węzłów (CDB) w pamięci RAM.
2. Friend sam wchodzi do własnej sieci, na adres `0x0001`.
3. Friend włącza własną rolę Friend (`bt_mesh_friend_set`).
4. Friend nasłuchuje beaconów nieprovisionowanych węzłów. Friend reaguje
   tylko na jeden znany UUID (LPN z tego projektu).
5. Gdy Friend wykryje ten UUID, Friend provisionuje węzeł PB-ADV na stały
   adres `0x0002`.
6. Friend konfiguruje LPN zdalnie (Config Client): dodaje AppKey, binduje
   modele Sensor Server i Health Server, ustawia publikację na adres
   Frienda.

Jeśli LPN był już wcześniej w CDB (np. po restarcie LPN-a), Friend usuwa
stary wpis i czyści listę ochrony przed powtórkami (RPL) przed nowym
provisioningiem.

### LPN 

LPN wykonuje te kroki po starcie:

1. LPN wystawia beacon nieprovisionowany i czeka na provisioning od
   Frienda.
2. Po provisioningu Friend konfiguruje LPN zdalnie (patrz wyżej).
3. LPN sprawdza co 2 sekundy, czy konfiguracja jest kompletna.
4. Po wykryciu kompletnej konfiguracji LPN czeka dodatkowe 8 sekund. Ten
   zapas chroni przed wejściem w tryb LPN w trakcie retransmisji Frienda.
5. LPN włącza tryb Low Power (`bt_mesh_lpn_set`) i szuka Frienda.
6. Po nawiązaniu friendship LPN zaczyna publikować pomiary temperatury w
   ustalonym interwale.

### Friendship - komunikacja LPN i Frienda

Friendship ma dwa etapy: nawiązanie i pracę ciągłą. LPN inicjuje oba
etapy. Friend tylko odpowiada.

**Nawiązanie friendship:**

1. LPN wysyła Friend Request (broadcast). Friend Request zawiera żądany
   PollTimeout i kryteria wyboru Frienda (minimalna wielkość kolejki,
   waga RSSI, waga okna odbioru).
2. Każdy Friend w zasięgu, który spełnia kryteria, odpowiada Friend
   Offer. Friend Offer zawiera zaoferowane okno odbioru (ReceiveWindow)
   i wielkość kolejki.
3. LPN wybiera najlepszą ofertę i wysyła pierwszy Friend Poll do
   wybranego Frienda. Friendship jest nawiązane po odebraniu odpowiedzi
   na ten Poll.

**Praca ciągła (po nawiązaniu):**

1. LPN śpi - radio jest wyłączone przez większość czasu.
2. LPN budzi się co PollTimeout i wysyła Friend Poll
   (`CONFIG_BT_MESH_LPN_POLL_TIMEOUT`, domyślnie 600 = 60 s w tym
   projekcie).
3. Po wysłaniu Polla LPN czeka ReceiveDelay
   (`CONFIG_BT_MESH_LPN_RECV_DELAY`, domyślnie 100 ms). Ten czas daje
   Friendowi czas na przygotowanie odpowiedzi.
4. LPN włącza odbiornik na czas ReceiveWindow. Friend deklaruje długość
   tego okna w Friend Offer (`CONFIG_BT_MESH_FRIEND_RECV_WIN`, 50 ms w
   tym projekcie). LPN kończy odbiór, gdy dostanie poprawną odpowiedź -
   nie czeka na koniec okna.
5. Friend odpowiada Friend Update. Friend Update niesie też zakolejkowaną
   wiadomość, jeśli Friend miał coś do przekazania LPN-owi.
6. Jeśli Friend ma w kolejce więcej niż jedną wiadomość, ustawia bit
   "More Data" w odpowiedzi. LPN wysyła kolejny Poll natychmiast, żeby
   odebrać resztę kolejki.

Każdy Poll i każda odpowiedź niosą 1-bitowy numer FSN (Friend Sequence
Number). FSN pozwala Friendowi odróżnić nowy Poll od retransmisji
zgubionego Polla.

**Publikacja LPN-a (uplink) nie używa Polla.** LPN wysyła własne
wiadomości (Sensor Status) jako normalny broadcast, niezależnie od
cyklu Poll. Friend zawsze nasłuchuje radia, więc odbiera taką wiadomość
od razu. Poll służy tylko do odbierania danych OD Frienda (downlink) -
LPN nie potrzebuje Polla do wysyłania własnych pomiarów.

**Zerwanie friendship.** Jeśli LPN nie wyśle Polla przez czas
PollTimeout, Friend uznaje friendship za zerwane i czyści kolejkę dla
tego LPN-a. LPN dostaje wtedy callback `lpn_terminated` (patrz
`app/apps/lpn/src/main.c`).

## Aplikacje (`app/apps/`)

| Aplikacja | Płytka | Rola | Czujnik temperatury | Uwagi |
|---|---|---|---|---|
| `friend` | nRF52840 Dongle | Friend + Provisioner + Sensor Client | - (odbiera pomiar) | Zasilany z USB, logi na UART |
| `lpn` | BTZ_EndDevice (nRF54L15) | LPN | Realny (STS4X) | Praca ciągła, System ON |
| `lpn_mock` | BTZ_EndDevice (nRF54L15) | LPN | Sztuczny (stała wartość) | Jak `lpn`, fake-owe pomiary |
| `lpn_off` | BTZ_EndDevice (nRF54L15) | LPN | Realny (STS4X) | Tryb System OFF między pomiarami |
| `lpn_off_mock` | BTZ_EndDevice (nRF54L15) | LPN | Sztuczny (stała wartość) | Jak `lpn_off`, fake-owe pomiary |


Repozytorium zawiera piątą aplikację, `baseline`, mierzy tylko poziom prądu
płytki bez radia, służy jako punkt odniesienia w pomiarach energii.

## Budowanie i flashowanie

### Friend

```bash
west build -b nrf52840dongle/nrf52840 -p always -d build/friend app/apps/friend
west flash -d build/friend
```

### LPN (System ON)

```bash
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p always -d build/lpn app/apps/lpn
west flash -d build/lpn --erase
```

### LPN Mock (System ON, bez czujnika)

```bash
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p always -d build/lpn_mock app/apps/lpn_mock
west flash -d build/lpn_mock --erase
```

### LPN Off (System OFF)

Bez retencji RAM:

```bash
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p always -d build/lpn_off app/apps/lpn_off
west flash -d build/lpn_off --erase
```

Z retencją RAM:

```bash
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p always -d build/lpn_off_ret app/apps/lpn_off -- -DEXTRA_CONF_FILE=overlay-ret.conf
west flash -d build/lpn_off_ret --erase
```

### LPN Off Mock (System OFF, bez czujnika)

Bez retencji RAM:

```bash
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p always -d build/lpn_off_mock app/apps/lpn_off_mock
west flash -d build/lpn_off_mock --erase
```

Z retencją RAM:

```bash
west build -b BTZ_EndDevice/nrf54l15/cpuapp -p always -d build/lpn_off_mock_ret app/apps/lpn_off_mock -- -DEXTRA_CONF_FILE=overlay-ret.conf
west flash -d build/lpn_off_mock_ret --erase
```

Gotowe zadania budowania i flashowania są również w `.vscode/tasks.json` (`Ctrl+Shift+P`).

## Ograniczenia

- **Węzły są bezstanowe.** `CONFIG_BT_SETTINGS=n` na wszystkich węzłach.
  Nic nie jest zapisywane na flash. Restart Frienda usuwa całą sieć i CDB
  - Friend odtwarza je od zera przy kolejnym starcie.
- **Restart LPN-a nie wymaga akcji użytkownika.** Po restarcie LPN
  wystawia beacon ponownie. Friend wykrywa znany UUID, usuwa stary wpis z
  CDB i provisionuje LPN od nowa na `0x0002`.
- **Cztery aplikacje LPN dzielą jeden adres.** `lpn`, `lpn_mock`,
  `lpn_off` i `lpn_off_mock` nigdy nie mogą działać w sieci jednocześnie.
  Flashuj na płytkę LPN tylko jedną z nich na raz.
- **Friendship nie przeżywa cyklu System OFF.** `lpn_off` i
  `lpn_off_mock` gubią friendship przy każdym wejściu w System OFF. Każda
  pobudka wymaga nowego Friend Requestu.
- **Zasięg provisioningu PB-ADV jest ograniczony.** Domyślne ustawienia
  Zephyra wysyłają dane provisioningu tylko raz (`retransmit=0`). Ten
  projekt podnosi ten parametr do 7 powtórzeń (`CONFIG_BT_MESH_PB_ADV_TRANS_PDU_RETRANSMIT_COUNT`)
  i używa pełnej mocy nadawania.
- **CDB ma miejsce na 2 węzły.** `CONFIG_BT_MESH_CDB_NODE_COUNT=2` -
  jeden slot na Frienda, jeden na LPN-a. Dodanie drugiego LPN-a wymaga
  zmiany tej wartości.
