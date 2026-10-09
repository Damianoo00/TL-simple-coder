# Budowanie i uruchamianie

Projekt wymaga systemu POSIX (np. Linux), GNU Make oraz GCC z obsługą C++20.

```sh
make -j2
make run
```

Makefile tworzy dwa oddzielne programy:

- `graph_worker` — uruchamia graf i czeka na SIGTERM lub SIGINT;
- `supervisor` — uruchamia trzy procesy `graph_worker`, po 5 sekundach wysyła
  do nich SIGTERM i odbiera ich zakończenie przez `waitpid`.

Bezpośrednie uruchomienie z katalogu projektu:

```sh
./supervisor ./graph_worker
```

Można podać bezwzględną ścieżkę workera, aby uruchomić supervisor z innego katalogu:

```sh
/home/damian/Documents/cpp_testy/async_nodes/supervisor /home/damian/Documents/cpp_testy/async_nodes/graph_worker
```

Bez argumentu supervisor używa `./graph_worker` względem bieżącego katalogu.
Ctrl+C lub SIGTERM skraca oczekiwanie i powoduje zamknięcie procesów potomnych.
Błąd uruchomienia workera jest wypisywany na stderr; supervisor zwraca kod 1
i zamyka wcześniej uruchomione procesy.

Sam worker:

```sh
./graph_worker
# Zakończ przez Ctrl+C.
```

`make clean` usuwa pliki budowania i utworzone programy.

Graf ma kolejność `InputNode -> Node -> Node`. Nazwę pliku przekazuje się przed
startem przez `input.configure({{"filename", "input.txt"}})`; konstruktor
`InputNode` nie przyjmuje argumentów. `InputNode::process()` odczytuje
jedną liczbę całkowitą typu `int` z kolejnej linii `input.txt` i wysyła ją do
kolejki następnego węzła. Plik jest otwierany względem bieżącego katalogu;
uruchamiaj program z katalogu projektu. Każdy z trzech workerów czyta cały plik
niezależnie. Dołączony plik zawiera liczby od 1 do 8. Przy otwieraniu pliku
worker wypisuje jego pełną ścieżkę. Zapisz zmiany pliku w edytorze przed
uruchomieniem; zmiana danych nie wymaga ponownej kompilacji.

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
- Klasa `Supervisor` zarządza startem i zamykaniem procesów, ale nie restartuje
  workera po awarii ani nie sprawdza jego kodu zakończenia. `stopAll()` czeka
  bez limitu czasu, więc zakleszczony worker może zablokować zamykanie.
- Jest to własny supervisor w C++, niezależny od zewnętrznego `supervisord`.
