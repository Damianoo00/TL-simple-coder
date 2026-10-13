# Budowanie i uruchamianie

Projekt wymaga systemu POSIX (np. Linux), CMake 3.20 lub nowszego,
kompilatora z obsługą C++20, Conana 2 oraz Pythona 3 do testów.
Zależności `nlohmann_json/3.12.0`, `crowcpp-crow/1.3.2` i `ffmpeg/7.1.5` są zadeklarowane
w `conanfile.txt`. Generatory `CMakeDeps` i `CMakeToolchain` przygotowują
pakiety i konfigurację dla CMake.

```sh
conan profile detect # Tylko przy pierwszym użyciu Conana, jeżeli nie ma profilu default.
conan install . --output-folder=build/conan --build=missing -s build_type=Release
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=conan/conan_toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
cmake --build build --target run
```

CMake tworzy dwa oddzielne programy w katalogu `build`:

- `graph_worker` — uruchamia graf i czeka na SIGTERM lub SIGINT;
- `supervisor` — udostępnia API HTTP w Crow, uruchamia osobny proces
  `graph_worker` dla każdego POST i odbiera zakończone procesy przez `waitpid`.

Opcje profilu i ustawień przekazuj do `conan install`, np. `-pr:h=default`.
Dla konfiguracji Debug ustaw `-s build_type=Debug` w Conanie oraz
`-DCMAKE_BUILD_TYPE=Debug` w CMake; użyj osobnego katalogu budowania.
Po zmianie profilu lub toolchaina użyj nowego katalogu budowania.
Testy można wyłączyć opcją CMake `-DBUILD_TESTING=OFF`.

Bezpośrednie uruchomienie z katalogu projektu:

```sh
./build/supervisor ./build/graph_worker
```

Można podać bezwzględną ścieżkę workera, aby uruchomić supervisor z innego katalogu:

```sh
/home/damian/Documents/cpp_testy/async_nodes/build/supervisor /home/damian/Documents/cpp_testy/async_nodes/build/graph_worker
```

Bez argumentu supervisor używa `./graph_worker` względem bieżącego katalogu.
Supervisor działa do Ctrl+C lub SIGTERM i wtedy zamyka procesy potomne.
Wysyła SIGTERM, czeka do 2 sekund na każdego workera, a w razie potrzeby
używa SIGKILL. Błąd `posix_spawn` daje odpowiedź HTTP 500.

## API Supervisora

Domyślnie API nasłuchuje na `127.0.0.1:8080`. Port możesz podać jako drugi argument:

```sh
./build/supervisor ./build/graph_worker 9090
```

Supervisor nie uruchamia grafów automatycznie. Wyślij istniejącą konfigurację:

```sh
curl -i -X POST http://127.0.0.1:8080/graphs \
  -H 'Content-Type: application/json' \
  --data-binary @config.json

curl http://127.0.0.1:8080/graphs
curl http://127.0.0.1:8080/graphs/1
curl -X DELETE http://127.0.0.1:8080/graphs/1
```

| Endpoint | Wynik |
|---|---|
| `POST /graphs` | `202 Accepted`, `graph_id`, `pid`, `status: starting`, nagłówek `Location` |
| `GET /graphs` | Lista wszystkich grafów z bieżącej sesji, także zakończonych |
| `GET /graphs/{id}` | Status procesu i opcjonalnie `exit_code` lub `signal`; 404 dla nieznanego ID |
| `DELETE /graphs/{id}` | Zatrzymanie procesu i jego status; powtórzenie jest bezpieczne; 404 dla nieznanego ID |

POST przyjmuje bezpośrednio dokument JSON w formacie `config.json`, bez dodatkowego
opakowania. Zła składnia lub struktura grafu daje 400, niewłaściwy Content-Type
415, a body powyżej 1 MiB — 413 (limit jest sprawdzany po odebraniu body).
Walidacja w supervisorze nie otwiera ani nie nadpisuje plików węzłów.
Konfiguracja jest zapisywana w prywatnym katalogu tymczasowym i przekazywana jako
argument do workera. Po zakończeniu procesu plik konfiguracji jest usuwany.

`202` potwierdza utworzenie procesu, a nie gotowość grafu. Status `running` oznacza,
że proces jeszcze się nie zakończył; nie jest potwierdzeniem zakończenia inicjalizacji.
Błąd konfiguracji zasobów (np. brak pliku wejściowego) powoduje status `failed`
z kodem zakończenia. `stopped` oznacza poprawne zakończenie lub zatrzymanie przez API.
Szczegóły błędów workera trafiają na stderr Supervisora. Rejestr oraz ID są
przechowywane w pamięci i zaczynają się od nowa po restarcie.

Ścieżki plików węzłów nadal są rozwiązywane względem katalogu roboczego Supervisora.
Przy wielu grafach nadaj każdemu osobny plik wyjściowy. API jest lokalne i nie
wymaga uwierzytelnienia; konfiguracje mają dostęp do plików z uprawnieniami procesu.

Sam worker:

```sh
./build/graph_worker
# Zakończ przez Ctrl+C.

# Alternatywny plik konfiguracji:
./build/graph_worker /ścieżka/do/config.json
```

`cmake --build build --target clean` usuwa skompilowane pliki i programy,
pozostawiając konfigurację CMake oraz zależności Conana.

Graf jest tworzony przez `JsonGraphParser::parseFile()` na podstawie `config.json`.
Metoda zwraca `std::unique_ptr<Graph>`; węzły są skonfigurowane i połączone,
ale ich wątki jeszcze nie pracują. Domyślna konfiguracja ma kolejność
`InputNode -> OutputNode`. `InputNode::process()` odczytuje jeden
`AVPacket` przez `av_read_frame()` i przekazuje go jako `DATAFRAME` do kolejki
następnego węzła. Przed danymi wysyła konfiguracje, a na końcu EOF dla każdego
strumienia.
Domyślne ścieżki to `input.mp4` i `output.mp4`, rozwiązywane względem bieżącego
katalogu. Przygotuj plik wejściowy przed uruchomieniem workera. Każdy worker
czyta cały plik niezależnie. Zmiana danych lub konfiguracji nie wymaga
ponownej kompilacji.

Konfiguracja zawiera niepustą tablicę `graph`. Każdy węzeł ma unikalną,
niepustą nazwę `name`, `type` (`InputNode`, `OutputNode`, `DemuxNode`, `MuxerNode`,
`VideoDecoderNode`, `AudioDecoderNode`, `VideoEncoderNode` lub `AudioEncoderNode`),
tablicę `configuration` obiektów z parametrami tekstowymi oraz tablicę
`connections` z nazwami następnych węzłów lub obiektami mapowania portów;
pusta tablica oznacza brak połączeń wychodzących.
`InputNode` i `OutputNode` wymagają niepustego parametru `filename`.
Powtarzające się klucze między obiektami `configuration` są odrzucane.
`InputNode` i `MuxerNode` mają jedno wyjście, `OutputNode` nie ma wyjść,
a `DemuxNode` ma konfigurowalną liczbę wyjść. Połączenia mogą wskazywać węzły opisane dalej w pliku.
Parser sprawdza strukturę i wszystkie odwołania przed konfiguracją węzłów;
błąd konfiguracji jest wypisywany na stderr, a worker kończy się kodem 1.
Ścieżka konfiguracji oraz względne ścieżki `filename` są rozwiązywane względem
bieżącego katalogu procesu. Węzły są dodawane w kolejności tablicy `graph`.

Koniec pliku kończy wątek wejściowy; proces workera nadal czeka na sygnał
zamknięcia. Brak pliku lub niepoprawny plik multimedialny powoduje błąd
konfiguracji i zakończenie workera z kodem 1.

Do zapisu MP4 służy `OutputNode` (nagłówek `nodes/OutputNode.hpp`).
Dodaj go jako ostatni węzeł i połącz z poprzednim przed startem grafu:

```cpp
OutputNode& output = graph.addNode<OutputNode>();
output.configure({{"filename", "output.mp4"}});
graph.connect(node2, output);
```

Konfiguracja tworzy plik lub nadpisuje jego dotychczasową zawartość.
`OutputNode::process()` odbiera `MediaPacket`, a dla `DATAFRAME` przelicza znaczniki czasu i zapisuje
pakiet przez `av_interleaved_write_frame()`. Błąd otwarcia zgłasza wyjątkiem,
a błąd zapisu komunikatem na stderr i zakończeniem wątku. Konfigurację należy
wykonać przed startem. Przy pracy wielu workerów używaj osobnego pliku dla
każdego procesu, aby uniknąć nadpisywania wyników.

## Pliki MP4

`InputNode` otwiera plik przez FFmpeg, a `OutputNode` zapisuje kontener MP4.
Przykład konfiguracji jest w `examples/mp4.json`; ustaw ścieżki do pliku
wejściowego i wyjściowego:

```sh
./build/graph_worker examples/mp4.json
```

Wejście odczytuje opis strumieni i skompresowane pakiety przez `libavformat`.
Kolejki przenoszą `MediaPacket` z polami `type`, `stream_id`, `config` i `packet`:

- `CONFIGFRAME` — pole `config` zawiera `MediaStream` z parametrami kodeka i `time_base` danego strumienia;
- `DATAFRAME` — pole `packet` zawiera `AVPacketPtr`, czyli `std::shared_ptr<AVPacket>` zwalniany przez `av_packet_free()`;
- `ENDFRAME` — EOF danego strumienia, bez danych.

`InputNode` wysyła wszystkie konfiguracje przed danymi, pakiety przeplata
w kolejności zwracanej przez FFmpeg, a na końcu wysyła EOF dla każdego ID:

```text
CONFIGFRAME(0), CONFIGFRAME(1), DATAFRAME(0), DATAFRAME(1), ...,
ENDFRAME(0), ENDFRAME(1)
```

Przeplatanie nie wymaga ścisłej naprzemienności: audio i wideo mogą mieć różną
liczbę pakietów. ID wejścia odpowiada `AVPacket::stream_index`. Wyjście mapuje
ID wiadomości na własne indeksy strumieni; ID nie muszą być kolejnymi liczbami.
Wszystkie konfiguracje muszą poprzedzać pierwszy `DATAFRAME` lub `ENDFRAME`.
Wyjście odrzuca nieznane ID, powtórzone konfiguracje/EOF, późne konfiguracje
oraz dane po EOF danego strumienia. Po EOF wszystkich strumieni finalizuje MP4.

`InputNode` i `OutputNode` dziedziczą bezpośrednio po abstrakcyjnym `NodeBase`.
Wejście rejestruje wyjście `MediaPacket`, a wyjście rejestruje wejście
`MediaPacket`. `ThreadSafeQueue<T>` przenosi typowane wiadomości i dziedziczy
po `QueueBase`, co pozwala sprawdzać typy połączeń dynamicznych.
Konfiguracja, dane i EOF przechodzą przez tę samą kolejkę.
Parametry FFmpeg i zarządzanie jego pamięcią są w `nodes/FFmpeg.hpp`;
`core/Node.hpp` nie zależy od FFmpeg. `int` zwracany z `process()` jest kodem stanu:
`NodeBase::Success` (0) oznacza kontynuację, `Finished` (-1) zakończenie,
a wartości poniżej -1 oznaczają błędy (nazwane kody w `NodeBase::Result`).
`configure()` również zwraca kod: przed uruchomieniem grafu należy sprawdzić,
czy zwróciło `Success`. Konstruktor muxera/demuxera z zerową liczbą portów
zapisuje `InvalidPorts`, dostępny przez `status()`. Błąd konfiguracji lub
przetwarzania pozostaje zapisany; kolejne `process()` zwraca ten sam kod.
Poprawna konfiguracja resetuje błąd. Konfigurację wykonuj przed startem,
a `status()` odczytuj przed startem lub po `stop()`.
Log na stderr podaje typ noda, kod, port i ID strumienia; błędy FFmpeg
zawierają także oryginalny kod, opis i ścieżkę pliku przy operacjach plikowych.
Parser JSON sprawdza wynik konfiguracji i zgłasza błąd z nazwą noda.
Wyjątki parsera, walidacji połączeń i alokacji standardowych kontenerów
pozostają poza tą zmianą. Kolejki nadal blokują i alokują pamięć, a logowanie
jest synchroniczne; ta zmiana nie zapewnia deterministycznego czasu wykonania.
Wyjście
przepisuje wszystkie strumienie do kontenera MP4, zachowując kodeki, audio
oraz znaczniki czasu. To remuksowanie, bez dekodowania klatek, modyfikacji
obrazu i ponownego kodowania. Kodeki muszą być obsługiwane przez kontener MP4.
Opcje Conana wyłączają dodatkowe biblioteki kodeków i urządzenia, których
remuksowanie nie potrzebuje.

Po wysłaniu wszystkich `ENDFRAME` wejście zamyka kolejkę. Każdy węzeł po zakończeniu
pracy zamyka swoje kolejki wyjściowe. Wyjście zapisuje
końcowy indeks MP4 po EOF wszystkich strumieni i zamyka plik; worker nadal
czeka na sygnał. Zatrzymanie grafu przed EOF zamyka kolejki bez generowania
sztucznego EOF; wyjście finalizuje wtedy odebrane dane jako częściowy plik.
Plik wyjściowy jest nadpisywany przy konfiguracji; używaj innej ścieżki niż
plik wejściowy. Testy integracyjne wymagają programów `ffmpeg` i `ffprobe` w PATH
(w przeciwnym razie są pomijane).

## Uwagi do kodu

- Oba pliki `api/GraphWorker.cpp` i `api/Supervisor.cpp` zawierają `main()`, dlatego
  muszą być linkowane do oddzielnych programów.
- `ThreadSafeQueue` po zamknięciu pozwala odebrać pozostałe elementy, a następnie
  zwraca `std::nullopt`. Kolejki nie można ponownie otworzyć.
- `NodeBase` i `Graph` są obecnie przeznaczone do pojedynczego cyklu start/stop.
  Rejestrację portów, `connect()` i konfigurację należy wykonywać przed startem;
  zmiana połączeń podczas pracy nie jest synchronizowana.
- `Graph::stop()` zamyka węzły w kolejności dodania. Aby opróżnić cały potok,
  dodawaj węzły od źródła do odbiornika, jak w obecnym przykładzie. Dowolna
  kolejność lub cykle wymagają odrębnego mechanizmu zamykania grafu.
- Klasa `Supervisor` synchronizuje rejestr procesów, odbiera zakończone dzieci
  i udostępnia ich kod zakończenia lub sygnał. Nie restartuje workera po awarii.
- Jest to własny supervisor w C++, niezależny od zewnętrznego `supervisord`.

## Własne węzły

Nowy węzeł dziedziczy bezpośrednio po `NodeBase` i implementuje `process()`.
W konstruktorze rejestruje swoje kolejki przez `addInput<T>()` i `addOutput<T>()`.
Przykładowy węzeł może mieć wejście `int` i wyjście `std::string`.
Dane odbiera przez `input<T>()->pop()` i wysyła przez `output<T>()->push()`.
Niepołączone wyjście zwraca pusty wskaźnik; sprawdź go, jeśli połączenie jest
opcjonalne. `NodeBase` zarządza wątkiem, kolejkami i propagacją ich zamknięcia.

```cpp
auto& input = graph.addNode<InputNode>();
auto& output = graph.addNode<OutputNode>();
graph.connect(input, output);
```

`connect()` i `Graph::connect()` sprawdzają zgodność typów kolejek w czasie
działania. Parser JSON obsługuje konkretne typy `InputNode`, `OutputNode`, `DemuxNode`
i `MuxerNode` oraz cztery nody dekodowania/kodowania; typ `Node` jest odrzucany. Walidacja odrzuca połączenia do wejścia `InputNode`
i z wyjścia `OutputNode` przed otwarciem plików.
W destruktorze własnego węzła wywołaj `stop()` przed zniszczeniem jego zasobów,
jak robią `InputNode` i `OutputNode`.

## Rozdzielanie i łączenie strumieni

`DemuxNode` ma jedno wejście i domyślnie dwa wyjścia. Kolejne konfiguracje
przypisuje do kolejnych portów: pierwsze ID do portu 0, drugie do portu 1 itd.
ID nie musi odpowiadać numerowi portu ani być kolejną liczbą. Wiadomości
`CONFIGFRAME`, `DATAFRAME` i `ENDFRAME` danego ID trafiają zawsze do tego samego
wyjścia. Po EOF strumienia jego kolejka jest zamykana. Niewykorzystane porty
są zamykane po zakończeniu konfiguracji, aby odbiorcy nie czekali bez końca.
Zbyt mała liczba wyjść lub brak połączenia dla aktywnego strumienia daje błąd.

`MuxerNode` ma domyślnie dwa wejścia i jedno wyjście. Każde wejście przenosi
jeden strumień. Najpierw odbiera i wysyła wszystkie konfiguracje w kolejności
portów, potem przeplata dane według DTS (zastępczo PTS) z uwzględnieniem
`time_base`. Pakiety bez obu znaczników są wybierane w kolejności portów.
ID oraz zawartość pakietów są zachowywane. EOF wszystkich skonfigurowanych
strumieni wysyła po ostatnich danych. Nie odtwarza dokładnej kolejności
nadejścia pakietów z wejściowego pliku — tworzy kolejność według czasu.

Muxer czeka na pierwszy pakiet lub EOF każdego aktywnego wejścia, żeby
porównać czasy. Wszystkie wejścia należy połączyć; port bez strumienia musi
mieć zamkniętą kolejkę (DemuxNode robi to automatycznie). Zatrzymanie grafu
zamyka kolejki i pozwala zakończyć oczekiwanie. Zamknięcie bez `ENDFRAME`
finalizuje częściowy potok bez generowania sztucznego EOF.

Liczbę portów określa tekstowy parametr `ports`, np. `{"ports": "3"}`,
albo argument konstruktora w C++:

```cpp
auto& demux = graph.addNode<DemuxNode>(2);
auto& mux = graph.addNode<MuxerNode>(2);
graph.connect(input, demux);
graph.connect(demux, mux, 0, 0);
graph.connect(demux, mux, 1, 1);
graph.connect(mux, output);
```

W JSON połączenie może być nazwą węzła (numer wyjścia odpowiada pozycji
w tablicy, wejście ma numer 0) albo obiektem z jawnymi portami:

```json
"connections": [
  {"node": "mux", "output_port": 0, "input_port": 0},
  {"node": "mux", "output_port": 1, "input_port": 1}
]
```

Parser sprawdza zakresy portów, powtórzone połączenia i kompletność wejść
MuxerNode przed otwarciem plików. Przykład całego potoku
`InputNode -> DemuxNode -> MuxerNode -> OutputNode` znajduje się
w `examples/demux_mux.json`.

### Dekodowanie i kodowanie

`VideoDecoderNode` i `AudioDecoderNode` przyjmują `MediaPacket` i zwracają
`MediaFrame` zawierający surowy `AVFrame`. `VideoEncoderNode` i
`AudioEncoderNode` przyjmują `MediaFrame` i zwracają `MediaPacket`.
Każdy z tych nodów ma jedno wejście, jedno wyjście i obsługuje jeden strumień.
Wszystkie zachowują ID strumienia oraz protokół CONFIGFRAME/DATAFRAME/ENDFRAME.
Surowa konfiguracja opisuje format pikseli lub próbek, wymiary lub układ kanałów,
time base oraz opcjonalny frame rate. Codec ID w konfiguracji surowej opisuje
kodek źródłowy, a enkoder wybiera własny kodek wyjściowy.

Dekoder otwiera kodek po CONFIGFRAME; konfigurację wyjściową wysyła przed
pierwszą zdekodowaną ramką, kiedy znany jest rzeczywisty format. Pusty strumień
również otrzymuje CONFIGFRAME i ENDFRAME. Przed ENDFRAME nody opróżniają bufory
kodeka, w tym opóźnione klatki B. Zamknięcie kolejki bez ENDFRAME oznacza
anulowanie: bufory są zwalniane bez generowania sztucznego EOF.

| Parametr | Dekodery | Enkoder wideo | Enkoder audio |
| --- | --- | --- | --- |
| `threads` | dodatnia liczba, domyślnie `1` | domyślnie `1` | domyślnie `1` |
| `codec` | wybierany z CONFIGFRAME | domyślnie AVC/H.264 (`libx264`) | domyślnie `aac` |
| `bit_rate` | — | dodatnia liczba bit/s, domyślnie `2000000` | domyślnie `128000` |
| `gop_size` | — | dodatnia liczba, domyślnie `12` | — |
| `max_b_frames` | — | `0`–`16`, domyślnie `0` | — |

W JSON wszystkie wartości konfiguracji są stringami. Kodek musi być dostępny
w buildzie FFmpeg; Conan włącza libx264 dla domyślnego kodowania AVC/H.264
i wyłącza m.in. libx265. AAC korzysta z natywnego enkodera FFmpeg.
Nieznane opcje, błędne wartości, niewłaściwy typ medium oraz niepoprawne ramki
zwracają kod błędu i log. Parser odrzuca połączenia pakietów z wejściami ramek
oraz połączenia dekodera audio z enkoderem wideo (i odwrotnie) przed otwarciem plików.

Enkodery zachowują format wejściowy: nie skalują obrazu ani nie konwertują
pikseli/próbek, częstotliwości czy kanałów. Nieobsługiwany format jest błędem.
Enkoder audio buforuje próbki w AVAudioFifo, dopasowuje wielkość ramek do kodeka
oraz wysyła końcowy niepełny blok (lub uzupełnia go ciszą, jeśli kodek wymaga
pełnej ramki). Znaczniki audio muszą być ciągłe; brakujący początkowy PTS
jest zastępowany zerem. Time base audio wynosi `1/sample_rate`, a wideo
zachowuje wejściowy time base (dla MPEG-4 mianownik jest ograniczony do 65535).
PTS i czas trwania ramek są przeliczane na time base enkodera; frame rate służy
do uzupełniania czasu trwania pakietów wideo, jeśli kodek zwraca zero.

Przykład pełnego transkodu jest w `examples/transcode.json`:
`InputNode -> DemuxNode -> (VideoDecoderNode -> VideoEncoderNode,
AudioDecoderNode -> AudioEncoderNode) -> MuxerNode -> OutputNode`.
Demux przypisuje porty według kolejności konfiguracji w pliku: przykład zakłada
wideo na porcie 0 i audio na porcie 1. Jeżeli plik ma odwrotną kolejność,
zamień połączenia demuxera. Uruchomienie: `./build/graph_worker examples/transcode.json`.

Nody używają [API send/receive FFmpeg](https://ffmpeg.org/doxygen/7.1/group__lavc__encdec.html).
Obecne kolejki, bufory, FFmpeg i synchroniczne logi nadal nie gwarantują
ściśle ograniczonego czasu wykonania.
