#include <iostream>
#include <vector>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ncurses.h>
#include <ctime>
#include <cstdlib>
#include <algorithm>
#include <clocale>

// =============================================================
// =======   STAŁE MAKSYMALNE (Limity tablic)  =================
// =============================================================

#define MAX_RUNWAYS 5
#define MAX_GATES 10
#define MAX_TANKERS 5

#define FUEL_MAX 20000
#define FUEL_NEEDED 600
#define FUEL_DELIVERY 6000
#define DELIVERY_TIME 12

#define LOG_HISTORY_SIZE 22

// Klucze IPC
#define SHM_KEY 77793  // Zmieniony klucz dla pewności odświeżenia
#define SEM_KEY 88903

#define SEM_PAS 0
#define SEM_GATE 1
#define SEM_CYSTERNA 2
#define MUTEX_ZASOBY 3

const char KIERUNKI_NAZWY[] = {'N', 'E', 'S', 'W'};

// =============================================================
// =======  PAMIĘĆ WSPÓŁDZIELONA Z KONFIGURACJĄ  ===============
// =============================================================

struct SharedData {
    // --- KONFIGURACJA (Ustawiana na starcie) ---
    int cfg_runways;        // Aktywne pasy
    int cfg_gates;          // Aktywne bramki
    int cfg_spawn_rate;     // Co ile cykli nowy samolot
    int cfg_pax_rate;       // Szansa na pasażera %
    int cfg_boarding_time;  // Czas postoju
    int cfg_plane_capacity; // Pojemność samolotu
    int cfg_landing_time;   // Czas na pasie (mikrosekundy)
    char scenariusz_nazwa[50]; // Nazwa do wyświetlania

    // --- STAN SYMULACJI ---
    int paliwo_w_magazynie;
    time_t nastepna_dostawa;
    int aktywne_samoloty;   // Licznik samolotów w systemie

    // Tablice o stałym rozmiarze MAX
    int pasy_startowe[MAX_RUNWAYS];
    int stanowiska_gate[MAX_GATES];
    int cysterny_status[MAX_TANKERS];

    int pasazerowie_w_terminalu[4]; // N, E, S, W

    // Informacje o samolotach w bramkach
    int gate_kierunek[MAX_GATES];
    int gate_liczba_pasazerow[MAX_GATES];

    // Logi
    char historia_logow[LOG_HISTORY_SIZE][60];
    int log_index;
};

int shmid = -1;
int semid = -1;
SharedData* shared_memory = nullptr;

// =============================================================
// =======  NARZĘDZIA (Semafore, Logi, Cleanup)  ===============
// =============================================================

void sem_p(int sem_num) {
    struct sembuf s = { (unsigned short)sem_num, -1, 0 };
    semop(semid, &s, 1);
}

void sem_v(int sem_num) {
    struct sembuf s = { (unsigned short)sem_num, 1, 0 };
    semop(semid, &s, 1);
}

void dodaj_log(const char* format, int id, char kierunek, int pasazerowie, const char* status) {
    sem_p(MUTEX_ZASOBY);
    char bufor[60];
    snprintf(bufor, 60, format, id, kierunek, pasazerowie, shared_memory->cfg_plane_capacity, status);
    int idx = shared_memory->log_index;
    strncpy(shared_memory->historia_logow[idx], bufor, 60);
    shared_memory->log_index = (idx + 1) % LOG_HISTORY_SIZE;
    sem_v(MUTEX_ZASOBY);
}

void cleanup(int signum) {
    endwin();
    if (shared_memory != nullptr) shmdt(shared_memory);
    if (shmid != -1) shmctl(shmid, IPC_RMID, nullptr);
    if (semid != -1) semctl(semid, 0, IPC_RMID);
    kill(0, SIGKILL);
    exit(0);
}

// =============================================================
// =======  PROCESY (Paliwo, Samolot)  =========================
// =============================================================

void proces_dostawcy_paliwa() {
    while(true) {
        shared_memory->nastepna_dostawa = time(NULL) + DELIVERY_TIME;
        sleep(DELIVERY_TIME);
        sem_p(MUTEX_ZASOBY);
        shared_memory->paliwo_w_magazynie += FUEL_DELIVERY;
        if (shared_memory->paliwo_w_magazynie > FUEL_MAX) shared_memory->paliwo_w_magazynie = FUEL_MAX;
        sem_v(MUTEX_ZASOBY);
    }
}

void proces_samolotu(int id) {
    srand(getpid() + id);

    // Rejestracja samolotu
    sem_p(MUTEX_ZASOBY);
    shared_memory->aktywne_samoloty++;
    sem_v(MUTEX_ZASOBY);

    sem_p(MUTEX_ZASOBY);
    int moj_kierunek = rand() % 4;
    sem_v(MUTEX_ZASOBY);
    // ---------------------

    // 1. LĄDOWANIE
    sem_p(SEM_PAS);
    int moj_pas = -1;
    int ilosc_pasow = shared_memory->cfg_runways;
    int start_node = rand() % ilosc_pasow;

    for(int i=0; i<ilosc_pasow; i++) {
        int idx = (start_node + i) % ilosc_pasow;
        if (shared_memory->pasy_startowe[idx] == 0) {
            shared_memory->pasy_startowe[idx] = id;
            moj_pas = idx;
            break;
        }
    }
    if (moj_pas == -1) {
        for(int i=0; i<ilosc_pasow; i++) if(shared_memory->pasy_startowe[i]==0) { shared_memory->pasy_startowe[i]=id; moj_pas=i; break; }
    }

    usleep(shared_memory->cfg_landing_time);

    // 2. PARKOWANIE
    sem_p(SEM_GATE);
    if(moj_pas != -1) shared_memory->pasy_startowe[moj_pas] = 0;
    sem_v(SEM_PAS);

    int my_gate_index = -1;
    int ilosc_bramek = shared_memory->cfg_gates;
    start_node = rand() % ilosc_bramek;

    for(int i=0; i<ilosc_bramek; i++) {
        int idx = (start_node + i) % ilosc_bramek;
        if(shared_memory->stanowiska_gate[idx] == 0) {
            shared_memory->stanowiska_gate[idx] = id;
            shared_memory->gate_kierunek[idx] = moj_kierunek;
            shared_memory->gate_liczba_pasazerow[idx] = 0;
            my_gate_index = idx;
            break;
        }
    }

    if (my_gate_index == -1) {
        sem_p(MUTEX_ZASOBY);
        shared_memory->aktywne_samoloty--;
        sem_v(MUTEX_ZASOBY);
        sem_v(SEM_GATE);
        exit(1);
    }

    // 3. TANKOWANIE
    sem_p(SEM_CYSTERNA);
    int my_tanker_index = -1;
    start_node = rand() % MAX_TANKERS;

    for(int i=0; i<MAX_TANKERS; i++) {
        int idx = (start_node + i) % MAX_TANKERS;
        if(shared_memory->cysterny_status[idx] == 0) {
            shared_memory->cysterny_status[idx] = id;
            my_tanker_index = idx;
            break;
        }
    }

    sem_p(MUTEX_ZASOBY);
    if (shared_memory->paliwo_w_magazynie >= FUEL_NEEDED) {
        shared_memory->paliwo_w_magazynie -= FUEL_NEEDED;
    }
    sem_v(MUTEX_ZASOBY);

    sleep(2);
    if (my_tanker_index != -1) shared_memory->cysterny_status[my_tanker_index] = 0;
    sem_v(SEM_CYSTERNA);

    // 4. BOARDING
    sleep(shared_memory->cfg_boarding_time);

    sem_p(MUTEX_ZASOBY);
    int capacity = shared_memory->cfg_plane_capacity;
    int ludzie_w_terminalu = shared_memory->pasazerowie_w_terminalu[moj_kierunek];

    int do_zabrania = (ludzie_w_terminalu < capacity) ? ludzie_w_terminalu : capacity;

    if (do_zabrania > 0) {
        shared_memory->pasazerowie_w_terminalu[moj_kierunek] -= do_zabrania;
        shared_memory->gate_liczba_pasazerow[my_gate_index] = do_zabrania;
    }
    int final_pax = shared_memory->gate_liczba_pasazerow[my_gate_index];
    sem_v(MUTEX_ZASOBY);

    const char* status = (final_pax >= capacity) ? "PELNY" : "ODLOT";
    dodaj_log("ID:%03d [%c] Pax: %d/%d (%s)", id, KIERUNKI_NAZWY[moj_kierunek], final_pax, status);

    // 5. ODLOT
    if (my_gate_index != -1) {
        shared_memory->stanowiska_gate[my_gate_index] = 0;
        shared_memory->gate_kierunek[my_gate_index] = -1;
        shared_memory->gate_liczba_pasazerow[my_gate_index] = 0;
    }
    sem_v(SEM_GATE);

    sem_p(SEM_PAS);
    moj_pas = -1;
    start_node = rand() % ilosc_pasow;

    for(int i=0; i<ilosc_pasow; i++) {
        int idx = (start_node + i) % ilosc_pasow;
        if (shared_memory->pasy_startowe[idx] == 0) {
            shared_memory->pasy_startowe[idx] = -id;
            moj_pas = idx;
            break;
        }
    }
    if (moj_pas == -1) {
        for(int i=0; i<ilosc_pasow; i++) if(shared_memory->pasy_startowe[i]==0) { shared_memory->pasy_startowe[i] = -id; moj_pas=i; break; }
    }

    usleep(shared_memory->cfg_landing_time);

    if(moj_pas != -1) shared_memory->pasy_startowe[moj_pas] = 0;
    sem_v(SEM_PAS);

    sem_p(MUTEX_ZASOBY);
    shared_memory->aktywne_samoloty--;
    sem_v(MUTEX_ZASOBY);

    exit(0);
}

// =============================================================
// =======  WIZUALIZACJA  ======================================
// =============================================================

void draw_interface(int loop_counter, int plane_id_counter, bool paused) {
    erase();
    int height = LINES;
    int width = 85;

    // --- TYTUŁ ---
    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(1, 2, " SYMULACJA LOTNISKA - %s", shared_memory->scenariusz_nazwa);
    attroff(A_BOLD | COLOR_PAIR(3));
    mvhline(2, 1, ACS_HLINE, width-1);

    // --- PALIWO ---
    mvprintw(3, 2, "PALIWO:");
    int bar_width = 30;
    float fuel_ratio = (float)shared_memory->paliwo_w_magazynie / FUEL_MAX;
    int filled_len = (int)(fuel_ratio * bar_width);
    mvprintw(3, 10, "[");
    if (fuel_ratio < 0.2) attron(COLOR_PAIR(2)); else attron(COLOR_PAIR(1));
    for (int i = 0; i < bar_width; i++) addch(i < filled_len ? ACS_CKBOARD : ' ');
    attroff(COLOR_PAIR(1) | COLOR_PAIR(2));
    printw("] %d L", shared_memory->paliwo_w_magazynie);

    // --- PASY ---
    int aktywne_pasy = shared_memory->cfg_runways;
    for(int i=0; i<aktywne_pasy; i++) {
        int pid = shared_memory->pasy_startowe[i];
        mvprintw(5 + i, 2, "PAS %d: ", i + 1);
        if (pid == 0) {
            attron(COLOR_PAIR(1)); printw("[ WOLNY ]"); attroff(COLOR_PAIR(1));
        } else if (pid > 0) {
            attron(COLOR_PAIR(2) | A_BLINK); printw("[ LADOWANIE ID:%d ]", pid); attroff(COLOR_PAIR(2) | A_BLINK);
        } else {
            attron(COLOR_PAIR(4) | A_BOLD); printw("[ STARTUJE ID:%d ]", abs(pid)); attroff(COLOR_PAIR(4) | A_BOLD);
        }
    }
    // Awaria pasa
    if (aktywne_pasy < 2 && shared_memory->cfg_runways == 1) {
        mvprintw(6, 2, "PAS 2: ");
        attron(COLOR_PAIR(5) | A_BOLD | A_BLINK);
        printw("[ !!! REMONT !!! ]");
        attroff(COLOR_PAIR(5) | A_BOLD | A_BLINK);
    }

    // --- CYSTERNY ---
    mvprintw(5, 45, "CYSTERNY:");
    for (int i = 0; i < MAX_TANKERS; ++i) {
        int pid = shared_memory->cysterny_status[i];
        mvprintw(6 + i, 45, "C%d: ", i + 1);
        if (pid == 0) { attron(COLOR_PAIR(1)); printw("[ WOLNA ]"); attroff(COLOR_PAIR(1)); }
        else { attron(COLOR_PAIR(4)); printw("[ ID:%d ]", pid); attroff(COLOR_PAIR(4)); }
    }

    // --- BRAMKI ---
    mvprintw(10, 2, "BRAMKI (%d czynnych):", shared_memory->cfg_gates);
    int aktywne_bramki = shared_memory->cfg_gates;
    for (int i = 0; i < aktywne_bramki; ++i) {
        int pid = shared_memory->stanowiska_gate[i];
        int kier = shared_memory->gate_kierunek[i];
        int pas = shared_memory->gate_liczba_pasazerow[i];
        int max_cap = shared_memory->cfg_plane_capacity;

        mvprintw(11 + i, 2, "G%-2d: ", i + 1);
        if (pid == 0) {
            attron(COLOR_PAIR(1)); printw("[ .................... ]"); attroff(COLOR_PAIR(1));
        } else {
            if (pas >= max_cap) attron(COLOR_PAIR(1)); else attron(COLOR_PAIR(4));
            printw("[ ID:%-3d ", pid);
            attron(A_BOLD); printw("%c", KIERUNKI_NAZWY[kier]); attroff(A_BOLD);
            printw(" %2d/%-2d ]", pas, max_cap);
            if (pas >= max_cap) attroff(COLOR_PAIR(1)); else attroff(COLOR_PAIR(4));
        }
    }

    // --- TERMINAL ---
    int term_y = 11 + aktywne_bramki + 1;
    // Jeśli bramki zachodzą nisko, przesuń terminal niżej
    if (term_y < 19) term_y = 19;

    mvhline(term_y - 1, 1, ACS_HLINE, width-1);
    mvprintw(term_y, 2, "TERMINAL (OCZEKUJACY):");
    int x_pos = 2;
    for(int i=0; i<4; i++) {
        mvprintw(term_y + 1, x_pos, "BRAMA %c: ", KIERUNKI_NAZWY[i]);
        int count = shared_memory->pasazerowie_w_terminalu[i];


        if (count >= shared_memory->cfg_plane_capacity)
            attron(COLOR_PAIR(2) | A_BOLD); // Czerwony
        else
            attron(COLOR_PAIR(1)); // Zielony

        printw("%-3d os.", count);
        attroff(COLOR_PAIR(1) | COLOR_PAIR(2) | A_BOLD);
        x_pos += 18;
    }

    // =========================================================
    // === RYSUNEK WIEŻY I SAMOLOTU (POD TERMINALEM) ===========
    // =========================================================
    // (Teraz term_y jest już zdefiniowane, więc nie będzie błędu)

    // =========================================================
    // === RYSUNEK WIEŻY I SAMOLOTU (POD TERMINALEM) ===========
    // =========================================================

    // art_y ustawiamy pod terminalem (term_y to linia nagłówka terminala)
    int art_y = term_y + 5;
    int art_x = 5;

    // --- WIEŻA KONTROLNA ---
    attron(A_BOLD | COLOR_PAIR(3)); // Cyjan
    mvprintw(art_y,     art_x, "      |~|      ");
    attron(A_BLINK); mvaddch(art_y, art_x + 6, '*'); attroff(A_BLINK); // Mrugające światło
    mvprintw(art_y + 1, art_x, "     [|_|]     ");
    mvprintw(art_y + 2, art_x, "    /     \\    ");
    mvprintw(art_y + 3, art_x, "   |_______|   ");
    mvprintw(art_y + 4, art_x, "     |   |     ");
    mvprintw(art_y + 5, art_x, "    /_____\\    ");
    attroff(A_BOLD | COLOR_PAIR(3));

    // --- NAPISY OBOK WIEŻY ---
    attron(A_BOLD);
    mvprintw(art_y + 1, art_x + 18, "SYSTEMY OPERACYJNE");
    mvprintw(art_y + 3, art_x + 18, "SYMULACJA LOTNISKA");
    attroff(A_BOLD);

    // --- SAMOLOT (TWÓJ PROJEKT) ---
    int plane_y = art_y + 6; // +6 żeby nie najechał na podstawę wieży

    attron(COLOR_PAIR(4) | A_BOLD); // Żółty/Biały
    // Używamy art_x (które wynosi 5), żeby pasowało do reszty
    mvprintw(plane_y,     art_x, "          __|__");
    mvprintw(plane_y + 1, art_x, "__________(_)__________");
    mvprintw(plane_y + 2, art_x, "   O   O       O   O");
    attroff(COLOR_PAIR(4) | A_BOLD);

    // =========================================================


    // =========================================================

    // --- ODLOTY (LOGI) ---
    int log_x = width + 2;
    attron(A_BOLD | COLOR_PAIR(3)); mvprintw(1, log_x, "HISTORIA ODLOTOW:"); attroff(A_BOLD | COLOR_PAIR(3));
    for(int i=0; i<LOG_HISTORY_SIZE; i++) {
        int idx = (shared_memory->log_index - 1 - i + LOG_HISTORY_SIZE) % LOG_HISTORY_SIZE;
        char* l = shared_memory->historia_logow[idx];
        if (strlen(l) > 0) {
            if (strstr(l, "PELNY")) attron(COLOR_PAIR(1)); else attron(COLOR_PAIR(4));
            mvprintw(3 + i, log_x, "%s", l);
            attroff(COLOR_PAIR(1) | COLOR_PAIR(4));
        }
    }

    // --- LEGENDA PARAMETRÓW I STATUSU ---
    // Rysujemy od dołu ekranu
    int info_y = height - 7;

    mvhline(info_y, 1, ACS_HLINE, width-1);

    // Parametry Scenariusza
    attron(A_REVERSE);
    mvprintw(info_y + 1, 2, " PARAMETRY SCENARIUSZA: ");
    attroff(A_REVERSE);

    mvprintw(info_y + 2, 2, "Spawn: %.1fs | Pax Rate: %d%% | Pojemnosc: %d",
             (float)shared_memory->cfg_spawn_rate / 10.0,
             shared_memory->cfg_pax_rate,
             shared_memory->cfg_plane_capacity);

    mvprintw(info_y + 3, 2, "Boarding: %ds | Ladowanie: %.1fs",
             shared_memory->cfg_boarding_time,
             (float)shared_memory->cfg_landing_time / 1000000.0);

    // Status
    mvhline(info_y + 4, 1, ACS_HLINE, width-1);

    attron(COLOR_PAIR(3));
    mvprintw(info_y + 5, 2, " Calkowita liczba lotow: %d", plane_id_counter - 1);
    attroff(COLOR_PAIR(3));



    attron(A_BOLD);
    mvprintw(info_y + 6, 2, "[ SPACJA ] = PAUZA / WZNOWIENIE   [ Q ] = WYJSCIE");
    attroff(A_BOLD);

    // --- PAUZA (WIELKI NAPIS) ---
    if (paused) {
        attron(COLOR_PAIR(5) | A_BOLD);
        int py = height / 2 - 2;
        int px = width / 2 - 22;
        mvprintw(py, px,     "                                             ");
        mvprintw(py+1, px,   "   !!! PAUZA - WCISNIJ SPACJE ABY WZNOWIC !!!   ");
        mvprintw(py+2, px,   "                                             ");
        attroff(COLOR_PAIR(5) | A_BOLD);
    }

    // RAMKI DOOKOŁA
    mvvline(0, width, ACS_VLINE, height);
    for(int i=0; i<width; i++) { mvaddch(0, i, ACS_HLINE); mvaddch(height-1, i, ACS_HLINE); }
    for(int i=0; i<height; i++) mvaddch(i, 0, ACS_VLINE);

    refresh();
}

// =============================================================
// =======  MAIN (MENU WYBORU)  ================================
// =============================================================

int main() {
    setlocale(LC_ALL, "");
    srand(time(NULL));

    // 1. CZYSZCZENIE
    int old_shmid = shmget(SHM_KEY, sizeof(SharedData), 0666);
    if (old_shmid != -1) shmctl(old_shmid, IPC_RMID, nullptr);
    int old_semid = semget(SEM_KEY, 4, 0666);
    if (old_semid != -1) semctl(old_semid, 0, IPC_RMID);

    // 2. TWORZENIE PAMIĘCI
    shmid = shmget(SHM_KEY, sizeof(SharedData), IPC_CREAT | 0666);
    shared_memory = (SharedData*)shmat(shmid, nullptr, 0);
    memset(shared_memory, 0, sizeof(SharedData));

    // 3. MENU WYBORU SCENARIUSZA
    std::cout << "============================================" << std::endl;
    std::cout << "    WYBIERZ SCENARIUSZ SYMULACJI LOTNISKA     " << std::endl;
    std::cout << "============================================" << std::endl;
    std::cout << "1. ZRÓWNOWAŻONY (Normalny ruch, wszystko dziala)" << std::endl;
    std::cout << "2. PARALIZ PASAZERSKI (Wakacje, tlumy ludzi)" << std::endl;
    std::cout << "3. AWARIA PASA (Tylko 1 pas dziala, zator)" << std::endl;
    std::cout << "Wybierz (1-3): ";

    int wybor;
    std::cin >> wybor;

    // USTAWIENIA DOMYŚLNE
    shared_memory->paliwo_w_magazynie = FUEL_MAX;
    shared_memory->nastepna_dostawa = time(NULL) + DELIVERY_TIME;
    shared_memory->aktywne_samoloty = 0;

    if (wybor == 1) {
        // Scenariusz 1: Ideał - Zrównoważony
        strcpy(shared_memory->scenariusz_nazwa, "SCENARIUSZ A: OPTYMALNY (BALANS)");
        shared_memory->cfg_runways = 2;
        shared_memory->cfg_gates = 6;

        // ZMIANY:
        shared_memory->cfg_spawn_rate = 35;
        shared_memory->cfg_pax_rate = 20;       // 20% - Szansa na pasażera (optymalna przy rzadszych lotach)
        shared_memory->cfg_boarding_time = 4;   // 5s - Czas na podziwianie postoju

        shared_memory->cfg_plane_capacity = 30; // Zwiększamy pojemność, bo samoloty są rzadziej
        shared_memory->cfg_landing_time = 2000000; // 2.0s - Szybsze lądowanie, żeby zwolnić pas
    }
    else if (wybor == 2) {
        strcpy(shared_memory->scenariusz_nazwa, "SCENARIUSZ B: TLUM");
        shared_memory->cfg_runways = 2;
        shared_memory->cfg_gates = 6;
        shared_memory->cfg_spawn_rate = 40;
        shared_memory->cfg_pax_rate = 80;
        shared_memory->cfg_boarding_time = 6;
        shared_memory->cfg_plane_capacity = 20;
        shared_memory->cfg_landing_time = 2000000;
    }
    else {
        strcpy(shared_memory->scenariusz_nazwa, "SCENARIUSZ C: AWARIA PASA");
        shared_memory->cfg_runways = 1;
        shared_memory->cfg_gates = 8;
        shared_memory->cfg_spawn_rate = 15;
        shared_memory->cfg_pax_rate = 10;
        shared_memory->cfg_boarding_time = 1;
        shared_memory->cfg_plane_capacity = 30;
        shared_memory->cfg_landing_time = 4000000;
    }

    // 4. INICJALIZACJA SEMAFORÓW
    semid = semget(SEM_KEY, 4, IPC_CREAT | 0666);
    semctl(semid, SEM_PAS, SETVAL, shared_memory->cfg_runways);
    semctl(semid, SEM_GATE, SETVAL, shared_memory->cfg_gates);
    semctl(semid, SEM_CYSTERNA, SETVAL, MAX_TANKERS);
    semctl(semid, MUTEX_ZASOBY, SETVAL, 1);

    signal(SIGINT, cleanup);

    if (fork() == 0) { proces_dostawcy_paliwa(); exit(0); }

    // 5. START GUI
    initscr();
    noecho();
    curs_set(0);
    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_RED, COLOR_BLACK);
    init_pair(3, COLOR_CYAN, COLOR_BLACK);
    init_pair(4, COLOR_YELLOW, COLOR_BLACK);
    init_pair(5, COLOR_YELLOW, COLOR_RED);

    nodelay(stdscr, TRUE);

    int loop_counter = 0;
    int plane_id_counter = 1;
    bool paused = false;

    while (true) {
        int ch = getch();

        if (ch == ' ') {
            paused = !paused;
            if (paused) {
                while(true) {
                    draw_interface(loop_counter, plane_id_counter, true);
                    usleep(50000);
                    int c2 = getch();
                    if (c2 == ' ') {
                        paused = false;
                        break;
                    }
                    if (c2 == 'q') { cleanup(0); }
                }
            }
        }
        else if (ch == 'q') break;

        if (!paused) {
            draw_interface(loop_counter, plane_id_counter, false);

            if (loop_counter % shared_memory->cfg_spawn_rate == 0) {
                if (fork() == 0) {
                    proces_samolotu(plane_id_counter);
                } else {
                    plane_id_counter++;
                }
            }

            if (rand() % 100 < shared_memory->cfg_pax_rate) {
                sem_p(MUTEX_ZASOBY);
                int kier = rand() % 4;
                shared_memory->pasazerowie_w_terminalu[kier] += (1 + rand() % 3);
                sem_v(MUTEX_ZASOBY);
            }
            loop_counter++;
        }

        while (waitpid(-1, NULL, WNOHANG) > 0);
        usleep(100000);
    }

    cleanup(0);
    return 0;
}