# Budowanie i uruchamianie

Projekt wymaga systemu POSIX (np. Linux), CMake 3.20 lub nowszego,
kompilatora z obsługą C++20, Conana 2 oraz Pythona 3 do testów.
Zależności `nlohmann_json/3.12.0` i `crowcpp-crow/1.3.2` są zadeklarowane
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
`InputNode -> Node -> OutputNode`. `InputNode::process()` odczytuje
jedną liczbę całkowitą typu `int` z kolejnej linii `input.txt` i wysyła ją do
kolejki następnego węzła. Plik jest otwierany względem bieżącego katalogu;
uruchamiaj program z katalogu projektu. Każdy uruchomiony worker czyta cały plik
niezależnie. Dołączony plik zawiera liczby od 1 do 8. Przy otwieraniu pliku
worker wypisuje jego pełną ścieżkę. Zapisz zmiany pliku w edytorze przed
uruchomieniem; zmiana danych lub konfiguracji nie wymaga ponownej kompilacji.

Konfiguracja zawiera niepustą tablicę `graph`. Każdy węzeł ma unikalną,
niepustą nazwę `name`, `type` (`InputNode`, `Node` albo `OutputNode`),
tablicę `configuration` obiektów z parametrami tekstowymi oraz tablicę
`connections` z nazwą następnego węzła lub pustą dla końca grafu.
`InputNode` i `OutputNode` wymagają niepustego parametru `filename`.
Powtarzające się klucze między obiektami `configuration` są odrzucane.
Obecne węzły mają jeden port wyjściowy, dlatego dozwolone jest najwyżej jedno
połączenie wychodzące. Połączenia mogą wskazywać węzły opisane dalej w pliku.
Parser sprawdza strukturę i wszystkie odwołania przed konfiguracją węzłów;
błąd konfiguracji jest wypisywany na stderr, a worker kończy się kodem 1.
Ścieżka konfiguracji oraz względne ścieżki `filename` są rozwiązywane względem
bieżącego katalogu procesu. Węzły są dodawane w kolejności tablicy `graph`.

Koniec pliku kończy wątek wejściowy; proces workera nadal czeka na sygnał
zamknięcia. Pusta linia, tekst lub wartość poza zakresem `int` powodują komunikat
z numerem linii i zakończenie odczytu. Brak pliku powoduje zakończenie workera
z kodem 1.

Do zapisu wartości do pliku służy `OutputNode` (nagłówek `OutputNode.hpp`).
Dodaj go jako ostatni węzeł i połącz z poprzednim przed startem grafu:

```cpp
OutputNode& output = graph.addNode<OutputNode>();
output.configure({{"filename", "output.txt"}});
graph.connect(node2, output);
```

Konfiguracja tworzy plik lub nadpisuje jego dotychczasową zawartość.
`OutputNode::process()` odbiera jedną liczbę z kolejki wejściowej, zapisuje ją
w osobnej linii i opróżnia bufor pliku. Błąd otwarcia zgłasza wyjątkiem,
a błąd zapisu komunikatem na stderr i zakończeniem wątku. Konfigurację należy
wykonać przed startem. Przy pracy wielu workerów używaj osobnego pliku dla
każdego procesu, aby uniknąć nadpisywania wyników.

## Uwagi do kodu

- Oba pliki `GraphWorker.cpp` i `Supervisor.cpp` zawierają `main()`, dlatego
  muszą być linkowane do oddzielnych programów.
- `ThreadSafeQueue` po zamknięciu pozwala odebrać pozostałe elementy, a następnie
  zwraca `std::nullopt`. Kolejki nie można ponownie otworzyć.
- `Node` i `Graph` są obecnie przeznaczone do pojedynczego cyklu start/stop.
  `init()`, `connect()` i konfigurację topologii należy wykonywać przed startem;
  zmiana połączeń lub `init()` podczas pracy nie są synchronizowane.
- `Graph::stop()` zamyka węzły w kolejności dodania. Aby opróżnić cały potok,
  dodawaj węzły od źródła do odbiornika, jak w obecnym przykładzie. Dowolna
  kolejność lub cykle wymagają odrębnego mechanizmu zamykania grafu.
- Klasa `Supervisor` synchronizuje rejestr procesów, odbiera zakończone dzieci
  i udostępnia ich kod zakończenia lub sygnał. Nie restartuje workera po awarii.
- Jest to własny supervisor w C++, niezależny od zewnętrznego `supervisord`.
