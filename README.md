# TL-simple-coder — Trybuna Live Simple Coder

## Struktura repozytorium

- `backend/` — silnik C++, API HTTP, testy oraz konfiguracja CMake i Conana.
- `frontend/` — edytor React / React Flow i konfiguracja Vite.
- `overlay-renderer/` — pomocniczy renderer Electron dla nakładek WWW.
- `examples/` — przykładowe grafy; `scripts/` — narzędzia developerskie.

Polecenia budowania i uruchamiania wykonuj z głównego katalogu repozytorium.
Programy backendu są zapisywane w `build/`, a frontend w `frontend/dist/`.

## Czym jest projekt

Trybuna Live Simple Coder to projekt C++20 do przetwarzania plików multimedialnych za pomocą
asynchronicznych grafów. Węzły odczytują, rozdzielają, dekodują, przekształcają,
kodują i zapisują strumienie audio oraz wideo, korzystając z FFmpeg.
Obsługiwane są remuksowanie MP4, transkodowanie, skalowanie obrazu,
resampling audio i nakładanie animowanych stron WWW na wideo.

Grafy można tworzyć w edytorze przeglądarkowym React / React Flow lub opisywać
w JSON. Supervisor udostępnia edytor i API HTTP, a każdy uruchomiony graf
przetwarza w osobnym procesie `graph_worker`.

## Docker Compose — budowanie i uruchomienie jedną komendą

Zainstaluj Docker Engine z Compose, a następnie w katalogu repozytorium wykonaj:

```sh
docker compose up
```

Otwórz **http://127.0.0.1:8080**. Pierwsze uruchomienie buduje backend C++,
uruchamia jego testy, testuje i buduje frontend oraz instaluje renderer Electron.
Pobranie i kompilacja FFmpeg mogą potrwać kilkanaście minut. Obraz zawiera Xvfb,
więc overlay nie wymaga sesji graficznej hosta. Serwer strony overlayu uruchom osobno.

Pliki wejściowe umieszczaj w katalogu `media/`. W edytorze ustawiaj ścieżki
kontenera, np. `/data/media/input.mp4` oraz `/data/media/output.mp4`.
Wyniki pojawią się w tym samym katalogu na hoście. Proces działa jako użytkownik
`node` (UID 1000); katalog musi umożliwiać mu odczyt i zapis.
Przykłady JSON wymagają dostosowania ścieżek plików; przykład overlayu również URL.
`127.0.0.1` w URL overlayu oznacza kontener. Do strony działającej na hoście
użyj np. `http://host.docker.internal:3000/overlay.html`; jej serwer musi nasłuchiwać
na interfejsie dostępnym z sieci Dockera.

Biblioteka grafów jest zachowywana w nazwanym wolumenie `graphs`, także po
`docker compose down`. `docker compose down -v` usuwa tę bibliotekę.
Ctrl+C zatrzymuje usługę i workery. API jest wystawione tylko na localhost hosta.

Po zmianach źródeł przebuduj obraz i uruchom usługę:

```sh
docker compose up --build
```

Konfiguracja budowania korzysta ze standardowego pola `build` w
[Docker Compose](https://docs.docker.com/reference/compose-file/build/).

## Jak zbudować lokalnie

Wymagania:

- system POSIX, np. Linux;
- CMake 3.20+, kompilator C++20 i Conan 2;
- Python 3 do testów (domyślnie włączonych);
- Node.js 22.12+ i npm do edytora oraz renderera overlayów;
- `ffmpeg` i `ffprobe` w PATH do testów integracyjnych mediów.

Zależności C++ (`nlohmann_json`, Crow i FFmpeg) są zadeklarowane
w [backend/conanfile.txt](backend/conanfile.txt). Z katalogu projektu wykonaj:

```sh
# Tylko przy pierwszym użyciu Conana, jeśli nie masz profilu default:
conan profile detect

conan install backend --output-folder=build/conan --build=missing -s build_type=Release
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=conan/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2

# Edytor przeglądarkowy:
npm ci --prefix frontend
npm run build --prefix frontend
```

CMake tworzy `build/supervisor` i `build/graph_worker`. Do pracy wyłącznie
z workerem lub API można pominąć budowanie edytora.

Dla Debug ustaw `-s build_type=Debug` w Conanie oraz
`-DCMAKE_BUILD_TYPE=Debug` w CMake. Użyj osobnego katalogu budowania,
również po zmianie profilu lub toolchaina. Opcje profilu przekazuj do
`conan install`, np. `-pr:h=default`. Testy można wyłączyć przez
`-DBUILD_TESTING=OFF` podczas konfiguracji CMake.

## Uruchomienie edytora

Z katalogu projektu:

```sh
./build/supervisor ./build/graph_worker
# Alternatywnie: cmake --build build --target run
```

Otwórz **http://127.0.0.1:8080**. Supervisor udostępnia edytor i API pod jednym
adresem. Inny port można podać jako drugi argument, np.:

```sh
./build/supervisor ./build/graph_worker 9090
```

Dodaj węzły z palety, połącz porty i ustaw parametry w prawym panelu.
`Zapisz` utrwala graf, `Start` zapisuje go i uruchamia, a `Stop` zatrzymuje.
Możesz również importować konfiguracje JSON i eksportować grafy wraz z pozycjami
węzłów. Dolny panel pokazuje logi ostatniego uruchomienia.

Edycja uruchomionego grafu dotyczy kolejnego startu. Niezapisane szkice pozostają
w pamięci przeglądarki do zamknięcia karty. Biblioteka grafów trafia do `graphs/`
względem katalogu roboczego supervisora; inny katalog wskazuje zmienna
`ASYNC_NODES_GRAPH_DIR`. Historia procesów i logi dotyczą bieżącej sesji.

Supervisor zamkniesz przez Ctrl+C; zatrzyma również procesy potomne.
API domyślnie działa lokalnie i nie wymaga uwierzytelnienia.

## Uruchomienie grafu z JSON

Przygotuj plik wejściowy i ustaw ścieżki `filename` w wybranej konfiguracji:

```sh
./build/graph_worker examples/mp4.json
```

Bez argumentu worker używa [config.json](config.json). Względne ścieżki są
rozwiązywane względem katalogu roboczego procesu. Plik wyjściowy jest nadpisywany:
używaj innej ścieżki niż wejściowa i osobnych plików dla równoległych grafów.
Worker kończy się automatycznie po zakończeniu grafu. Ctrl+C przerywa przetwarzanie.

| Przykład | Działanie |
| --- | --- |
| [mp4.json](examples/mp4.json) | Remuksowanie do MP4 bez ponownego kodowania |
| [demux_mux.json](examples/demux_mux.json) | Rozdzielenie i ponowne połączenie strumieni |
| [transcode.json](examples/transcode.json) | Transkodowanie wideo i audio do H.264/AAC |
| [transcode_scaled.json](examples/transcode_scaled.json) | Wideo 1280×720 i audio 48 kHz stereo |
| [overlay.json](examples/overlay.json) | Nakładanie strony WWW na wideo w czasie rzeczywistym |

Przykłady z demuxerem zakładają wideo na porcie 0 i audio na porcie 1.
Jeśli plik ma inną kolejność strumieni, dostosuj połączenia.

Graf można też uruchomić przez działający supervisor:

```sh
curl -i -X POST http://127.0.0.1:8080/graphs \
  -H 'Content-Type: application/json' \
  --data-binary @examples/mp4.json

curl http://127.0.0.1:8080/graphs
curl -X DELETE http://127.0.0.1:8080/graphs/1
```

Do zatrzymania użyj `graph_id` zwróconego przez API. Odpowiedź `202` oznacza
utworzenie procesu; błędy otwarcia mediów mogą pojawić się później w logach.

## Overlay WWW

Przed użyciem `VideoOverlayNode` zainstaluj renderer:

```sh
npm ci --prefix overlay-renderer
npm run setup --prefix overlay-renderer
```

Krok `setup` pobiera binarny Electron/Chromium. Na Linuksie renderer wymaga sesji
graficznej X11/Wayland lub Xvfb. Uruchom serwer strony, ustaw jej URL w konfiguracji
węzła i wykonaj:

```sh
./build/graph_worker examples/overlay.json
```

Jeżeli Chromium kończy pracę z `Failed to move to new namespace` i
`Operation not permitted`, środowisko blokuje namespace’y wymagane przez sandbox.
Sam plik `chrome-sandbox` z uprawnieniami setuid nie usuwa tej blokady.
W ograniczonym kontenerze można jawnie wyłączyć sandbox renderera:

```sh
ASYNC_NODES_OVERLAY_NO_SANDBOX=1 docker compose up --build
```

Dla uruchomienia lokalnego ustaw tę samą zmienną przed poleceniem workera lub
supervisora. Wartość `1` włącza przełącznik Chromium `--no-sandbox`; używaj tego
trybu wyłącznie do zaufanych stron overlayu, ponieważ wyłącza izolację procesów
Chromium. Domyślnie sandbox pozostaje włączony. Aby do niego wrócić w Compose,
uruchom `docker compose up -d` bez tej zmiennej.

Przykład używa `input.mp4`, zapisuje `build/overlay-output.mp4` i zakłada stronę
pod `http://127.0.0.1:3000/overlay.html`. Dla projektu Trybuna Live Overlays
można uruchomić serwer pomocniczym skryptem:

```sh
python3 scripts/start_trybuna_overlay.py /ścieżka/do/Trybuna-Live-Overlays
```

Skrypt przygotowuje kopię aplikacji w `build/trybuna-overlay`. Przetwarzanie
overlayu odbywa się w czasie rzeczywistym; gotowy plik potwierdza log
`finalized=yes reason=end_of_stream`.

## Praca nad projektem i testy

```sh
ctest --test-dir build --output-on-failure
npm test --prefix frontend
```

W trybie deweloperskim uruchom supervisor na porcie 8080, a w drugim terminalu:

```sh
npm run dev --prefix frontend
```

Edytor działa wtedy pod **http://127.0.0.1:5173** i przekazuje zapytania API do
supervisora. Po zmianach interfejsu używanego na porcie 8080 ponownie wykonaj
`npm run build --prefix frontend`.

Szczegóły testów i pomiaru pokrycia znajdują się w [backend/tests/README.md](backend/tests/README.md).
Architektura, protokoły węzłów, parametry konfiguracji i opis API są w
[AGENTS.md](AGENTS.md), wczytywanym automatycznie przez Codexa przy pracy w repozytorium.

# Transmisja RTMP

`RTMPOutputNode` przyjmuje zakodowane pakiety H.264 i AAC i wysyła je jako FLV do
serwera RTMP. W `examples/rtmp.json` ustaw `filename` pliku wejściowego oraz
`url` publikacji w postaci `rtmp://host:port/aplikacja/strumień`, a następnie uruchom:

```sh
./build/graph_worker examples/rtmp.json
```

Przykład zakłada, że plik wejściowy zawiera H.264 i AAC. Dla innych kodeków
użyj dekodera, enkodera H.264/AAC oraz `MuxerNode` przed `RTMPOutputNode`.
Opcja `realtime` ma wartość `true` domyślnie i odmierza czas według DTS/PTS,
więc plik jest nadawany w tempie rzeczywistym. Ustaw `"realtime": "false"`
tylko gdy odbiornik ma przyjąć pakiety tak szybko, jak powstają.
Połączenie sieciowe jest otwierane przy pierwszym pakiecie lub końcu strumienia,
po otrzymaniu konfiguracji. Błąd sieci zatrzymuje graf. Operacje sieciowe mają
limit pięciu sekund, a zatrzymanie grafu przerywa oczekiwanie.

### Zapętlanie pliku wejściowego

`InputNode` przyjmuje opcjonalny parametr tekstowy `loop`: `"true"` lub `"1"`
włącza nieskończone powtarzanie pliku, a `"false"` lub `"0"` wyłącza je
(domyślnie wyłączone). Przykład konfiguracji:

```json
"configuration": [{"filename": "input.mp4", "loop": "true"}]
```

Po końcu pliku wejście przewija materiał, zachowuje ID strumieni i przesuwa
PTS/DTS kolejnych powtórzeń o wspólny czas trwania. Konfiguracje są wysyłane
tylko raz; pomiędzy powtórzeniami nie ma ENDFRAME. Graf działa do zatrzymania
lub błędu. Wejście musi obsługiwać przewijanie i zawierać pakiety z możliwym
do ustalenia czasem trwania. Postęp odczytu zaczyna się od zera przy każdym
powtórzeniu. Zapętlenie nie ogranicza szybkości odczytu do czasu rzeczywistego.

### Uzupełnianie przerw w audio i wideo

Dwa osobne węzły obsługują przerwy w PTS zdekodowanych ramek, także na granicy
zapętlanego pliku, którego strumienie mają różne długości:

- `AudioGapFillerNode`: wstawia dokładną liczbę brakujących próbek ciszy.
  Umieść go **przed AudioResamplerNode**, np.
  `AudioDecoderNode → AudioGapFillerNode → AudioResamplerNode → AudioEncoderNode`.
- `VideoGapFillerNode`: wypełnia przerwę kopiami ostatniego obrazu, korzystając
  z czasu trwania klatki (lub frame rate z CONFIGFRAME, gdy czas nie jest podany).
  Umieść go za dekoderem, przed scalerem, overlayem lub enkoderem.

Oba węzły mają jedno wejście i wyjście `MediaFrame` i są dostępne w edytorze.
Opcjonalny tekstowy parametr `max_gap_ms` jest dodatnią liczbą milisekund,
domyślnie `"10000"`. Większa przerwa, brak PTS, zmiana formatu lub nakładanie
się ramek powodują błąd; audio toleruje zaokrąglenie o jedną próbkę, a wideo
nakładanie o jeden tick time base przy rosnącym PTS. Początkowe przesunięcie
strumienia jest zachowane. Węzły nie generują danych przed pierwszą ramką ani
po ostatniej: przerwę wykrywają dopiero po otrzymaniu kolejnej ramki, nie według
zegara ściennego. Nie wyrównują długości strumieni przy końcowym EOF.
Pusty strumień zachowuje CONFIGFRAME/ENDFRAME, anulowanie nie generuje EOF.

Przykład: `./build/graph_worker examples/transcode_gap_filled.json`.
Czyta `input.mp4` w pętli i zapisuje `build/transcode-gap-filled.mp4` do Ctrl+C.
Domyślna pojemność każdej kolejki wynosi 256 elementów. Pole `queue_capacity`
w JSON nadpisuje tę wartość, również w istniejących grafach.
`queue_capacity: 256` ogranicza buforowanie; dla innych przeplotów strumieni
limit należy dobrać osobno. Na wyjściu RTMP nadal ustaw `realtime: "true"`.
EOF kończy węzły normalnie; brak nowych ramek bez EOF nie uruchamia generatora
ciszy ani obrazu.
