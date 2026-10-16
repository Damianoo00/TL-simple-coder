# TL-simple-coder — Trybuna Live Simple Coder

## Czym jest projekt

Trybuna Live Simple Coder to projekt C++20 do przetwarzania plików multimedialnych za pomocą
asynchronicznych grafów. Węzły odczytują, rozdzielają, dekodują, przekształcają,
kodują i zapisują strumienie audio oraz wideo, korzystając z FFmpeg.
Obsługiwane są remuksowanie MP4, transkodowanie, skalowanie obrazu,
resampling audio i nakładanie animowanych stron WWW na wideo.

Grafy można tworzyć w edytorze przeglądarkowym React / React Flow lub opisywać
w JSON. Supervisor udostępnia edytor i API HTTP, a każdy uruchomiony graf
przetwarza w osobnym procesie `graph_worker`.

## Jak zbudować

Wymagania:

- system POSIX, np. Linux;
- CMake 3.20+, kompilator C++20 i Conan 2;
- Python 3 do testów (domyślnie włączonych);
- Node.js 22.12+ i npm do edytora oraz renderera overlayów;
- `ffmpeg` i `ffprobe` w PATH do testów integracyjnych mediów.

Zależności C++ (`nlohmann_json`, Crow i FFmpeg) są zadeklarowane
w [conanfile.txt](conanfile.txt). Z katalogu projektu wykonaj:

```sh
# Tylko przy pierwszym użyciu Conana, jeśli nie masz profilu default:
conan profile detect

conan install . --output-folder=build/conan --build=missing -s build_type=Release
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

Szczegóły testów i pomiaru pokrycia znajdują się w [tests/README.md](tests/README.md).
Architektura, protokoły węzłów, parametry konfiguracji i opis API są w
[AGENTS.md](AGENTS.md), wczytywanym automatycznie przez Codexa przy pracy w repozytorium.
