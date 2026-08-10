# BLE Mesh w pigułce

Ten dokument opisuje teorię standardu BLE Mesh. Ten dokument nie zawiera kodu.
Ten dokument tłumaczy warstwę radiową, adresowanie, model danych, bezpieczeństwo,
role węzłów i proces provisioningu. Ta wiedza pomaga zrozumieć decyzje
architektoniczne z README.md i wyniki pomiarów z Analiza-poboru-pradu.md.

**Dokumenty pokrewne:**

- [`README.md`](../README.md) — architektura tego projektu: role Friend i LPN,
  implementacja, ograniczenia.
- [`Analiza-poboru-pradu.md`](Analiza-poboru-pradu.md) — pomiary poboru prądu, wyniki,
  wnioski.

---

## 1. Po co powstał BLE Mesh

Bluetooth Low Energy w klasycznym trybie łączy dwa urządzenia. Ten tryb ma
jedno ograniczenie: zasięg jednego radia.

BLE Mesh usuwa to ograniczenie. Wiele urządzeń przekazuje sobie wiadomość
dalej. Zasięg sieci rośnie z liczbą urządzeń, nie tylko z mocą jednego
nadajnika.

Bluetooth SIG opublikował standard BLE Mesh w roku 2017. BLE Mesh używa tej
samej warstwy fizycznej co zwykłe BLE. BLE Mesh nie wymaga nowego chipu
radiowego.

Typowe zastosowanie BLE Mesh: oświetlenie w budynku, czujniki w magazynie,
automatyka przemysłowa. Te zastosowania mają wspólną cechę: dużo tanich
węzłów, krótkie wiadomości, brak potrzeby adresu IP.

---

## 2. Warstwa radiowa

BLE Mesh działa w paśmie 2,4 GHz. To samo pasmo używają Wi-Fi, Thread i
Zigbee.

BLE Mesh wysyła wiadomości przez trzy kanały advertising: 37, 38 i 39. Te
same kanały służą normalnie do rozgłaszania obecności zwykłego urządzenia
BLE. BLE Mesh używa ich do przenoszenia danych sieciowych.

Jedna wiadomość mieści się w jednym pakiecie advertising, bez segmentacji, do
29 bajtów danych. Dłuższa wiadomość wymaga segmentacji. Warstwa Transport
dzieli długą wiadomość na kilka segmentów. Odbiorca składa segmenty z
powrotem w jedną wiadomość.

---

## 3. Managed flooding — dlaczego BLE Mesh nie routuje

Sieci Thread i Zigbee budują tablice routingu. Każdy węzeł zna trasę do
celu.

BLE Mesh nie buduje tablic routingu. BLE Mesh używa innej metody: managed
flooding.

Managed flooding działa tak:

1. Węzeł nadaje wiadomość jako broadcast, do wszystkich w zasięgu.
2. Każdy węzeł w zasięgu odbiera tę wiadomość.
3. Węzeł z rolą **Relay** nadaje tę samą wiadomość dalej, znowu jako
   broadcast.
4. Wiadomość dociera do całej sieci. Nadawca nie musi znać trasy do
   odbiorcy.

Managed flooding potrzebuje ochrony przed niekończącą się pętlą
retransmisji. Dwa mechanizmy dają tę ochronę:

| Mechanizm | Działanie |
|---|---|
| **TTL** (Time To Live) | licznik w nagłówku wiadomości. Każdy Relay zmniejsza TTL o 1 przed retransmisją. Węzeł z TTL = 0 nie retransmituje dalej |
| **Message Cache** | lista ostatnio widzianych wiadomości na każdym węźle. Węzeł odrzuca wiadomość, którą już widział |

Managed flooding ma zaletę i wadę.

- **Zaleta:** prosty algorytm, brak tablic routingu, sieć działa mimo awarii
  pojedynczego węzła.
- **Wada:** ruch sieciowy rośnie z liczbą węzłów szybciej niż w sieci
  routowanej. Duża sieć BLE Mesh generuje dużo powtórzonych transmisji tej
  samej wiadomości.

Ten kompromis odróżnia BLE Mesh od Thread. Thread płaci za routing większą
złożonością protokołu. BLE Mesh płaci za prostotę gorszym skalowaniem
ruchu.

---

## 4. Adresowanie

BLE Mesh nie używa adresów IP. BLE Mesh używa własnych, 16-bitowych
adresów.

| Typ adresu | Zakres | Przeznaczenie |
|---|---|---|
| Unicast | 0x0001–0x7FFF | jeden konkretny element jednego węzła |
| Group | 0xC000–0xFFFF (i kilka adresów stałych, np. All-Nodes) | wielu odbiorców naraz, np. „wszystkie lampy w pokoju” |
| Virtual | 16-bitowy skrót ze 128-bitowego UUID | wielu odbiorców z różnych producentów, bez centralnej rejestracji adresu |
| Unassigned | 0x0000 | element bez przypisanego adresu — stan przed provisioningiem |

Provisioner przydziela adres unicast węzłowi podczas provisioningu. Węzeł z
wieloma elementami dostaje kolejne adresy unicast po kolei, jeden adres na
jeden element.

---

## 5. Model danych: Element, Model, State, Message

BLE Mesh opisuje funkcję urządzenia w czterech warstwach pojęciowych.

```
Node (jedno fizyczne urządzenie)
 └─ Element (jedna adresowalna część węzła, ma własny adres unicast)
      └─ Model (funkcja: Server trzyma stan, Client wysyła komendy)
           └─ State (aktualna wartość, np. On/Off, poziom jasności)
```

- **Node** — jedno fizyczne urządzenie po provisioningu.
- **Element** — logiczna część węzła. Prosty czujnik ma jeden element.
  Panel z dwoma przełącznikami ma dwa elementy.
- **Model** — zestaw stanów, wiadomości i zachowań dla jednej funkcji.
  Model **Server** trzyma stan i odpowiada na zapytania. Model **Client**
  wysyła komendy i odczytuje stan.
- **State** — aktualna wartość funkcji, np. temperatura albo stan
  włącz/wyłącz.
- **Message** — wiadomość z opcode i parametrami. Opcode identyfikuje typ
  wiadomości, np. `GET`, `SET`, `STATUS`.

Modele dzielą się na dwie grupy:

- **Modele SIG** — zdefiniowane przez Bluetooth SIG, np. Generic OnOff,
  Sensor Server, Health Server. Ten projekt używa modeli Sensor Server i
  Health Server (patrz README.md).
- **Modele Vendor** — zdefiniowane przez producenta, poza specyfikacją SIG.
  Producent dodaje własny opcode i własną semantykę.

### Publish/Subscribe

Model Server nie wysyła danych na sztywno zapisany adres. Model Server
**publikuje** dane na skonfigurowany adres. Model Client odbiera te dane,
gdy **subskrybuje** ten sam adres.

Publikacja i subskrypcja to konfiguracja, nie kod. Provisioner ustawia je
zdalnie przez model Config Client, po provisioningu.

---

## 6. Bezpieczeństwo: trzy warstwy kluczy

BLE Mesh szyfruje każdą wiadomość na dwóch warstwach: sieciowej i
aplikacyjnej. Trzy typy kluczy obsługują to szyfrowanie.

| Klucz | Zasięg | Rola |
|---|---|---|
| **NetKey** | cała podsieć | szyfruje warstwę Network. Węzeł bez właściwego NetKey nie widzi ruchu tej podsieci |
| **AppKey** | jedna aplikacja (np. oświetlenie) | szyfruje warstwę Access. Węzeł potrzebuje właściwego AppKey, żeby odczytać treść wiadomości danej aplikacji |
| **DevKey** | jeden konkretny węzeł | unikalny na węzeł, ustalony podczas provisioningu. Provisioner używa DevKey do prywatnej konfiguracji tego jednego węzła, np. do przesłania mu NetKey i AppKey |

Jedna podsieć może mieć wiele kluczy NetKey naraz, na przykład do podziału
sieci na strefy. Jeden węzeł może znać wiele kluczy AppKey, jeśli obsługuje
wiele aplikacji. Klucz DevKey nigdy nie opuszcza pary Provisioner-węzeł.
Inne węzły nie znają cudzego DevKey.

> W tym projekcie NetKey i AppKey są celowo stałe i jawne (patrz
> README.md). Ten wybór ułatwia podsłuch ruchu w Wireshark do celów
> analizy. Ten wybór obniża realne bezpieczeństwo sieci.

---

## 7. Role węzłów

BLE Mesh definiuje cztery role. Jeden węzeł może pełnić kilka ról naraz.

| Rola | Zasilanie | Radio | Zadanie |
|---|---|---|---|
| **Node** | dowolne | dowolne | rola bazowa — każdy węzeł po provisioningu jest Node |
| **Relay** | zwykle sieciowe | zawsze włączone | retransmituje wiadomości dalej, wydłuża zasięg sieci (sekcja 3) |
| **Proxy** | zwykle sieciowe | zawsze włączone, GATT | most między urządzeniem bez natywnego radia BLE Mesh (np. telefonem) a siecią advertising |
| **Low Power Node (LPN)** | bateria | wyłączone większość czasu | oszczędza energię, odbiera dane tylko przez okno Friend Poll |
| **Friend** | zwykle sieciowe | zawsze włączone | buforuje wiadomości dla jednego lub więcej LPN, oddaje je na żądanie |

### Relay i Proxy w tym projekcie

Ten projekt nie używa ról Relay ani Proxy. Ten wybór ma konkretny powód. Ten
wybór nie jest przeoczeniem.

Rola Relay rozszerza zasięg sieci na wiele przeskoków radiowych. Sieć w tym
projekcie ma tylko dwa węzły: Friend i LPN, w bezpośrednim zasięgu
radiowym. Dodatkowy przeskok nie jest tu potrzebny.

Brak roli Relay ma konsekwencję: wiadomość dociera tylko tak daleko, jak
sięga jeden skok radiowy. Sieć z tego projektu nie obsłuży węzła poza
zasięgiem Frienda bez dodania osobnego węzła z rolą Relay.

Rola Proxy służy urządzeniom bez natywnego stosu BLE Mesh advertising, na
przykład telefonom z aplikacją mobilną. Proxy tłumaczy ruch advertising na
GATT i z powrotem.

Ten projekt nie ma takiego urządzenia. Friend i LPN mają natywny stos BLE
Mesh. Friend i LPN rozmawiają bezpośrednio przez advertising bearer. Rola
Proxy jest tu zbędna.

Brak roli Proxy ma konsekwencję: telefon z generyczną aplikacją BLE Mesh
(np. nRF Mesh) nie zobaczy tej sieci bez dodania osobnego węzła Proxy.

---

## 8. Provisioning — dodawanie węzła do sieci

Provisioning to proces przyjęcia nowego, nieprovisionowanego urządzenia do
sieci mesh. Provisioner prowadzi ten proces. W tym projekcie rolę
Provisionera pełni Friend (patrz README.md).

### 8.1 Dwa bearery: PB-ADV i PB-GATT

Provisioning Bearer przenosi dane provisioningu między Provisionerem a
nowym urządzeniem. Istnieją dwa bearery.

| | PB-ADV | PB-GATT |
|---|---|---|
| Transport | pakiety advertising, bezpośrednio | usługa GATT nad połączeniem BLE |
| Kto używa | urządzenia z natywnym stosem BLE Mesh | telefony i systemy bez dostępu do surowego advertising provisioningu (np. iOS) |
| Segmentacja | Generic Provisioning Layer dzieli dane na PDU | standardowe mechanizmy GATT (zapis i powiadomienie ATT) |
| W tym projekcie | **używany** — Friend provisionuje LPN przez PB-ADV | nieużywany |

System iOS nie daje aplikacjom dostępu do surowego formatu pakietów
advertising provisioningu. Aplikacje na iOS dlatego używają PB-GATT.

Ten projekt provisionuje jedno urządzenie embedded przez drugie urządzenie
embedded. Oba urządzenia mają natywny stos BLE Mesh. PB-ADV wystarcza. PB-
ADV jest prostszy niż PB-GATT.

### 8.2 Fazy provisioningu

Provisioning ma pięć faz. Kolejność faz jest stała.

```
1. Beaconing            nieprovisionowane urządzenie rozgłasza swój UUID
        │
2. Invitation           Provisioner zaprasza urządzenie; urządzenie odsyła
        │               listę swoich możliwości
        │
3. Public Key Exchange  obie strony wymieniają klucze publiczne ECDH
        │
4. Authentication       obie strony potwierdzają tożsamość metodą OOB
        │
5. Provisioning Data    Provisioner wysyła zaszyfrowane dane sieciowe
                        (NetKey, adres, IV Index)
```

**Faza 1 — Beaconing.** Nieprovisionowane urządzenie wysyła beacon z
własnym, 128-bitowym UUID. Provisioner nasłuchuje beaconów. Provisioner
rozpoznaje znane urządzenia po UUID. W tym projekcie Friend reaguje tylko
na jeden, zapisany na sztywno UUID (patrz README.md).

**Faza 2 — Invitation.** Provisioner wysyła zaproszenie. Urządzenie
odpowiada listą własnych możliwości: liczbę elementów, obsługiwane
algorytmy, dostępne metody OOB.

**Faza 3 — Public Key Exchange.** Obie strony generują parę kluczy ECDH
(Elliptic Curve Diffie-Hellman). Obie strony wymieniają klucze publiczne.
Obie strony liczą ten sam sekret współdzielony, bez przesyłania go przez
radio. Ten sekret staje się podstawą dalszego szyfrowania sesji
provisioningu.

**Faza 4 — Authentication.** Ta faza broni przed atakiem
man-in-the-middle na wymianę kluczy publicznych. Obie strony potwierdzają
tożsamość jedną z metod OOB (Out-Of-Band — kanał poza radiem BLE).

| Metoda OOB | Jak działa | Przykład |
|---|---|---|
| No OOB | brak potwierdzenia, najniższe bezpieczeństwo | urządzenie bez ekranu i bez dodatkowych sensorów |
| Static OOB | stały klucz, zapisany fabrycznie w obu miejscach | klucz na naklejce urządzenia, wpisany ręcznie do aplikacji |
| Output OOB | urządzenie pokazuje wartość (np. miga diodą N razy); człowiek wpisuje tę wartość do Provisionera | dioda, głośnik, wyświetlacz |
| Input OOB | człowiek wpisuje wartość do urządzenia; Provisioner tę wartość zna | przycisk, klawiatura na urządzeniu |

W tym projekcie provisioning działa automatycznie, bez udziału człowieka.
Friend rozpoznaje LPN po UUID. Friend provisionuje LPN bez interakcji
człowieka. To działa jako wariant No OOB. Ten wybór pasuje do scenariusza
testowego. Ten wybór nie pasuje do scenariusza produkcyjnego.

**Faza 5 — Provisioning Data.** Provisioner szyfruje dane sieciowe kluczem
sesji z faz 3 i 4. Provisioner wysyła te dane do urządzenia:

- NetKey podsieci,
- adres unicast dla urządzenia,
- aktualny IV Index (licznik, chroni przed powtórką starych wiadomości),
- Key Refresh Flag.

Po fazie 5 urządzenie ma NetKey i adres. Urządzenie jest teraz węzłem
sieci. Urządzenie nie ma jeszcze AppKey. Urządzenie nie ma jeszcze
skonfigurowanych modeli. Provisioner konfiguruje to osobnym krokiem, przez
model Config Client (patrz README.md, sekcja Friend, punkt 6).

---

## 9. BLE Mesh na tle innych protokołów mesh

Ta sekcja daje krótkie zestawienie na poziomie standardu. Zmierzone różnice
w poborze prądu są w [Analiza-poboru-pradu.md](Analiza-poboru-pradu.md). Szersze
porównanie z Matter/Thread i Zigbee jest w projekcie `matter-end-device`
(`docs/results.md`, sekcja 3).

| | **BLE Mesh** | **Thread** | **Zigbee** |
|---|---|---|---|
| Radio | BLE, 2,4 GHz | 802.15.4, 2,4 GHz | 802.15.4, 2,4 GHz |
| Topologia | managed flooding, bez routingu | mesh routowany, samonaprawiający się | mesh routowany, koordynator |
| Adresowanie | własne, 16-bit, nie-IP | IPv6 natywnie | krótkie adresy 16-bit, nie-IP |
| Wyjście na zewnątrz | Proxy (most GATT) | router (Border Router) | bramka tłumacząca aplikację |
| Urządzenia bateryjne | LPN + Friend | SED/SSED | end devices |
| Pojedynczy punkt awarii | brak | brak (kilka Border Routerów, wybór Leadera) | koordynator jest krytyczny |
| Skalowanie ruchu przy dużej sieci | słabe (flooding powtarza ruch) | dobre (routing) | dobre (routing) |
| Wymagania sprzętowe | najniższe | wysokie (stos Thread + Matter razem) | średnie |

BLE Mesh wygrywa w tym scenariuszu: urządzenie ma i tak radio BLE do
parowania z telefonem, sieć jest mała lub średnia, priorytetem jest
prostota i niski koszt sprzętu.

BLE Mesh przegrywa w tym scenariuszu: duża sieć z wieloma przeskokami
(flooding generuje dużo powtórzonego ruchu), potrzeba natywnej integracji z
siecią IP.
