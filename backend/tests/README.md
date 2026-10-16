# Testy backendu

Wszystkie polecenia wykonuj z głównego katalogu repozytorium.

Testy C++ mają etykietę `unit`; testy Python uruchamiające procesy workera,
supervisora i narzędzia multimedialne mają etykietę `integration`.
Asercje C++ działają również w kompilacji Release (nie używają `assert`).

```sh
cmake --build build -j2
ctest --test-dir build -L unit --output-on-failure
ctest --test-dir build --output-on-failure
```

Test `overlay_node` używa lokalnego procesu imitującego renderer i gniazda Unix;
nie wymaga Electron ani serwera WWW. Testy integracyjne wymagają lokalnych gniazd
TCP. Wszystkie testy mają limit czasu w CTest, także na wypadek zakleszczenia.

## Zakres

| Zestaw | Sprawdzane zachowania |
| --- | --- |
| `thread_safe_queue` | FIFO, własność danych, zamykanie i opróżnianie, limity, odblokowanie producentów i konsumentów, wielu producentów i konsumentów |
| `node_lifecycle`, `typed_nodes` | zgodność portów, przetwarzanie i propagacja EOF, zatrzymanie, wyjątki, stabilność błędów grafu, anulowanie przy pełnych kolejkach |
| `json_graph_parser` | schemat JSON, granice pojemności i portów, kolizje połączeń, typy mediów, diagnostyka plików i węzłów, walidacja bez otwierania plików |
| `media_validation` | niepoprawne opcje, odzyskanie po błędzie konfiguracji, niepoprawne ramki i pakiety, terminalne kody błędów, anulowanie, funkcje pomocnicze |
| `output_protocol` | kolejność konfiguracji/danych/EOF, niepoprawne payloady, duplikaty, wiele strumieni, finalizacja i ponowna konfiguracja |
| `input_protocol` | rzeczywisty plik WAV tworzony w teście, zachowanie parametrów i próbek PCM, EOF, ponowna konfiguracja i odzyskanie po błędach |
| `demux_mux` | rozdzielanie strumieni, scalanie według czasu DTS, własność pakietów, nieużywane porty, błędy protokołu |
| `codec_nodes` | kodowanie/dekodowanie audio i wideo, opóźnione dane, puste strumienie, znaczniki czasu i anulowanie |
| `frame_conversion` | skalowanie i resampling, formaty, znaczniki czasu, flush i walidacja |
| `overlay_node` | transport ramek i kompozycja deterministycznej nakładki |

## Pomiar pokrycia jednostkowego (GCC/gcov)

Użyj nowego katalogu kompilacji, aby liczniki nie zawierały poprzednich uruchomień
ani testów integracyjnych. Poniższe komendy zakładają istniejące zależności Conana
w `build/conan`. Mapowanie Debug na Release dotyczy gotowych bibliotek Conana;
kod projektu jest kompilowany bez optymalizacji i z instrumentacją.

```sh
cmake -S . -B /tmp/async-nodes-unit-coverage \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/conan/conan_toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_MAP_IMPORTED_CONFIG_DEBUG=Release \
  '-DCMAKE_CXX_FLAGS=--coverage -O0' -DCMAKE_EXE_LINKER_FLAGS=--coverage
cmake --build /tmp/async-nodes-unit-coverage -j2
ctest --test-dir /tmp/async-nodes-unit-coverage -L unit --output-on-failure
python3 scripts/unit_coverage.py /tmp/async-nodes-unit-coverage
```

Skrypt podaje pokrycie linii dla `core`, `nodes` i `api`, w tym nagłówków.
Scala wyniki tej samej linii z różnych instancji szablonów i plików kompilacji.
Raport obejmuje także pliki wykonywalne API, jeśli je skompilowano; ich nieuruchomione
linie mają zerowe pokrycie. Pokrycie linii nie oznacza pokrycia wszystkich gałęzi
ani potwierdzenia braku wyścigów. Awarie alokacji i zewnętrznych bibliotek nie są
symulowane przez te testy.

Raport z GCC 15.2, Debug `-O0`, po uruchomieniu wyłącznie testów jednostkowych:
[coverage-unit.txt](coverage-unit.txt). `GRAPH_CORE` obejmuje rdzeń, węzły i parser;
`TOTAL` uwzględnia dodatkowo kod procesów API. Wynik zależy od wersji kompilatora.
Raport mierzy linie, nie gałęzie; nie ustalamy na jego podstawie automatycznego
progu jakości. Testy poprawnych danych i błędów pozostają podstawowym kryterium.
