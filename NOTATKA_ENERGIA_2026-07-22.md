# Notatka — optymalizacja poboru prądu węzła LPN (nRF54L15 / BTZ_EndDevice)

**Data:** 2026-07-22


---

## Cel

Węzeł LPN pobierał w spoczynku **~200 µA**, podczas gdy referencyjny „pusty" firmware (`build_idle`, sam `k_sleep`) na tej samej fizycznej płytce osiągał **2,49 µA**. Celem było zdiagnozowanie źródła nadmiarowego poboru i sprowadzenie węzła do poziomu bliskiego teoretycznemu minimum dla nRF54L15 w trybie System ON idle.

## Wynik

| Etap | Pobór w spoczynku (floor) |
|---|---|
| Stan wyjściowy | ~200 µA |
| Po usunięciu obsługi LED | ~65 µA |
| Po wyłączeniu nieużywanych czujników + bramkowaniu zasilania | **~3 µA** |

**Redukcja z ~200 µA do ~3 µA (ok. 65×).** Poziom zbliżony do referencyjnego `build_idle` (2,49 µA).

---

## Zdiagnozowane źródła nadmiarowego poboru

**1. Pin diody LED (P2.08) skonfigurowany jako wyjście — ~135 µA**
Samo ustawienie pinu LED jako wyjścia (nawet bez zapalania diody) dodawało na tej płytce ~135 µA. Węzeł jest czysto czujnikowy i dioda nie jest potrzebna. Potwierdzone porównaniem binarnym firmware'u i konfiguracji z `build_idle`.

**2. Nieużywany czujnik światła LTR329 (i akcelerometr BMI270) — regularne piki ~192 µA co 500 ms**
Płytka ma na magistrali I2C trzy czujniki (STS4X — temperatura, BMI270 — IMU, LTR329 — światło). Aplikacja odczytuje **tylko STS4X**, ale przy włączonym `CONFIG_SENSOR` Zephyr inicjalizuje przy starcie **każdy** czujnik zadeklarowany w devicetree. Sterownik LTR329 przełączał czujnik w tryb ciągłego pomiaru (domyślnie: pomiar co 500 ms), co dawało regularne „garby" ~192 µA co pół sekundy — mimo że dane z tego czujnika nigdy nie były używane.

**3. Szyna zasilania czujników trzymana na stałe — ~65 µA (dominujący składnik floora)**
Zasilanie czujników (bramka FET na P0.00) było w konfiguracji sprzętowej włączane na starcie i pozostawało włączone bez przerwy (`regulator-boot-on`), więc **wszystkie** czujniki na tej szynie (STS4X, BMI270, LTR329) ciągnęły prąd spoczynkowy 24/7, choć STS4X jest potrzebny tylko raz na 10 s. To ta stale załączona szyna odpowiadała za większość pozostałego floora ~65 µA.

---

## Co po kolei usuwaliśmy / zmienialiśmy w kodzie

Kolejność prac w ciągu dnia, każdy krok potwierdzony pomiarem PPK2:

1. **Usunięcie całej obsługi diody LED** z aplikacji `lpn` oraz `lpn_mock` — wycięcie konfiguracji pinu P2.08 jako wyjścia, sekwencji migania i sterowania GPIO w modelu Generic OnOff (model pozostał, ale bez fizycznego pinu). *Efekt: ~200 µA → ~65 µA.*

2. **Wyłączenie nieużywanych czujników LTR329 i BMI270** — dodanie nakładki devicetree w aplikacji `lpn`, która oznacza oba czujniki jako `disabled`, przez co Zephyr ich nie inicjalizuje. *Efekt: zniknęły regularne piki ~192 µA co 500 ms.*

3. **Bramkowanie szyny zasilania czujników** — zamiast trzymać zasilanie STS4X włączone na stałe, włączamy je tylko na czas pojedynczego odczytu temperatury (co 10 s), a poza tym utrzymujemy wyłączone. *Efekt: floor spadł do ~3 µA.*

4. **(porządkowo) Ujednolicenie wariantu testowego `lpn_mock`** z `lpn` — publikacja pomiarów przywiązana do nawiązania połączenia z węzłem Friend, tak samo w obu wariantach. Bez wpływu na pobór, dla spójności zachowania.

---

## Efekt uboczny (do świadomości, nieistotny energetycznie)

Bramkowanie zasilania powoduje, że przy każdym odczycie (co 10 s) włączenie szyny ładuje kondensatory odsprzęgające czujników — pojawia się krótki (~30 µs) prąd rozruchowy (inrush) rzędu ~100 mA. Na wykresie PPK2 wygląda to jak wysokie, nieregularne piki, ale przenoszony ładunek to ~3–4 µC, czyli uśrednione **~0,4 µA** — pomijalne wobec 65 µA zaoszczędzonych przez bramkowanie. Nieregularna wysokość pików to artefakt próbkowania (zdarzenie krótsze niż rozdzielczość czasowa miernika), nie zmiana w zachowaniu układu.

---

## Porównanie: `lpn` vs `lpn_mock`

W projekcie istnieją dwa warianty tego samego węzła LPN. **Nigdy nie pracują w sieci jednocześnie** — uruchamiane są osobno. Mają identyczny adres, klucze i identyczne zachowanie sieciowe mesh (samo-provisioning, tryb LPN, poll co 60 s, publikacja przywiązana do połączenia z węzłem Friend). Różni je **wyłącznie źródło temperatury**.

| | `lpn` (produkcyjny) | `lpn_mock` (testowy) |
|---|---|---|
| Źródło temperatury | realny czujnik STS4X (I2C) | wartość zmyślona (stałe 22,5 °C) |
| Czujniki fizyczne | STS4X czytany co 10 s | brak (`CONFIG_SENSOR`, I2C, regulator wyłączone) |
| Zastosowanie | docelowy firmware na urządzenie | testy sieci / analiza w snifferze bez potrzeby żywego czujnika |
| Ramka na antenie | identyczna jak mock | bajt-w-bajt taka sama jak lpn |
| **Pobór (uśredniony)** | **14,58 µA** | **8,67 µA** |

**Różnica 14,58 − 8,67 = 5,9 µA to w całości koszt odczytu realnego czujnika STS4X** (włączenie szyny zasilania, rozgrzanie, pomiar I2C). `lpn_mock` tego nie robi, dlatego jest lżejszy — pokazuje „czysty" koszt samej warstwy mesh LPN (radio: publikacja + poll).

Szacunkowa żywotność na baterii CR2032 (~180 mAh użytecznej, bez uwzględnienia samorozładowania):
- `lpn` @ 14,58 µA → **~1,4 roku**
- `lpn_mock` @ 8,67 µA → **~2,4 roku**

Rozbicie poboru `lpn`:

| Składnik | Pobór (uśredniony) | Występuje w |
|---|---|---|
| Floor (nRF54L15 System ON idle) | ~3 µA | lpn + mock |
| Radio: publikacja co 10 s + poll co 60 s | ~5,7 µA | lpn + mock |
| Odczyt STS4X co 10 s | ~5,9 µA | tylko lpn |

---

## Jak można jeszcze zmniejszyć pobór w LPN

Uszeregowane wg wielkości zysku:

**1. Interwał odczytu / publikacji — największy lewar (efekt liniowy).**
Obecnie temperatura jest czytana i wysyłana co 10 s. Wydłużenie interwału zmniejsza **jednocześnie** koszt radia i koszt czujnika. Przykładowo 10 s → 60 s to ~6× mniej dla obu tych składników — węzeł zszedłby w okolice **~5 µA**. To decyzja aplikacyjna: jak często realnie potrzebny jest odczyt temperatury.

**2. Niższa dokładność (repeatability) czujnika STS4X — WDROŻONE.**
Czujnik pracował w trybie „high" (pomiar 8,3 ms). Przełączony na tryb „low" (1,6 ms) — aktywny pomiar skrócony ~5×, przy praktycznie nieodczuwalnej dla monitoringu temperatury otoczenia utracie dokładności. Urywa istotną część z owych 5,9 µA kosztu czujnika.

**3. Skrócenie czasu rozgrzewania szyny czujnika — WDROŻONE.**
Po włączeniu zasilania czekaliśmy 5 ms na ustabilizowanie się czujnika; STS4X jest gotowy już po ~1 ms. Zapas skrócony do 2 ms — krótszy czas trzymania szyny włączonej.

**4. Poll timeout — już zoptymalizowany.**
Interwał podtrzymania połączenia z węzłem Friend wynosi 60 s (długi). Dalsze wydłużanie daje niewielki zysk, a opóźniałoby dostarczanie ewentualnych wiadomości z sieci do węzła.

**Wynik po wdrożeniu punktów 2 i 3:** pobór `lpn` spadł z **14,58 µA do 13,4 µA** (pomiar PPK2).

**Uwaga o dalszych krokach:** punkty 2 i 3 zostały wdrożone (zmiana parametrów, bez zmiany logiki). Pozostaje punkt 1 — daje największy efekt, ale wymaga ustalenia dopuszczalnego interwału raportowania temperatury (obecnie 10 s). Poniżej tego poziomu dalsza optymalizacja wymagałaby już zmian sprzętowych (np. soft-start szyny zasilania, dobór baterii pod kątem samorozładowania).

---

## Wnioski

- Główne źródła nadmiarowego poboru w tego typu firmware to **peryferia inicjalizowane „przy okazji"** (nieużywane czujniki z devicetree), **piny GPIO ustawione jako wyjścia** oraz **szyny zasilania trzymane na stałe**.
- Zmiany dotyczą wyłącznie aplikacji `lpn` (produkcyjnej) i `lpn_mock` (testowej); nie ruszaliśmy węzła Friend.
- Węzeł osiąga obecnie pobór spoczynkowy zbliżony do minimum sprzętowego platformy; dalsza optymalizacja wymagałaby już zmian sprzętowych (np. soft-start szyny zasilania).
