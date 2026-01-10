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
#include <cmath>
// =============================================================
// =======  USTAWIENIA "REALISTYCZNY CHAOS" (MIX WYNIKÓW) ======
// =============================================================

#define RUNWAYS_COUNT 2
#define GATES_COUNT 6
#define TANKERS_COUNT 3

#define FUEL_MAX 20000
#define FUEL_NEEDED 600
#define FUEL_DELIVERY 6000
#define DELIVERY_TIME 12

// --- PARAMETRY RUCHU ---
#define PLANE_CAPACITY 22     // Lekko niestandardowa liczba (trudniej trafić idealnie)
#define BOARDING_SPEED 4      // Średnie tempo (4 osoby/sek)
#define BOARDING_TIME 6       // Krótki czas! Samolot nie będzie czekał w nieskończoność.

// --- GENERATOR (NA STYK) ---
#define SPAWN_RATE 35         // Samolot co 3.5s (Zwiększamy presję, samoloty częściej!)
// To jest klucz do mieszanki wyników:
#define PASSENGER_RATE 25     // 25% szans (zmniejszone z 35%) - Mniejszy tłok
#define PASSENGER_GROUP_SIZE 6 // Ale jak przychodzą, to większymi grupami (1-6) - Duża zmienność!

#define LANDING_TIME 2500000  // 2.5s na pasie
#define TANKING_TIME 3        // 3s tankowania



// --- LOGOWANIE ---
#define LOG_HISTORY_SIZE 20

// =============================================================

#define SHM_KEY 77781 // Nowy klucz
#define SEM_KEY 88892

// --- INDEKSY SEMAFORÓW ---
#define SEM_PAS 0
#define SEM_GATE 1
#define SEM_CYSTERNA 2
#define MUTEX_ZASOBY 3

const char KIERUNKI_NAZWY[] = {'N', 'E', 'S', 'W'};

struct SharedData {
    int paliwo_w_magazynie;
    time_t nastepna_dostawa;

    int pasy_startowe[RUNWAYS_COUNT];
    int stanowiska_gate[GATES_COUNT];
    int cysterny_status[TANKERS_COUNT];

    int pasazerowie_w_terminalu[4];

    int gate_kierunek[GATES_COUNT];
    int gate_liczba_pasazerow[GATES_COUNT];

    char historia_logow[LOG_HISTORY_SIZE][60];
    int log_index;
};

int shmid = -1;
int semid = -1;
SharedData* shared_memory = nullptr;

// --- SEMAFORY ---
void sem_p(int sem_num) {
    struct sembuf s = { (unsigned short)sem_num, -1, 0 };
    semop(semid, &s, 1);
}

void sem_v(int sem_num) {
    struct sembuf s = { (unsigned short)sem_num, 1, 0 };
    semop(semid, &s, 1);
}

// --- FUNKCJA LOGUJĄCA ---
void dodaj_log(const char* format, int id, char kierunek, int pasazerowie, const char* status) {
    sem_p(MUTEX_ZASOBY);
    char bufor[60];
    snprintf(bufor, 60, format, id, kierunek, pasazerowie, PLANE_CAPACITY, status);
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

// --- DOSTAWCA ---
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

// --- SAMOLOT ---
void proces_samolotu(int id) {
    srand(getpid() + id);
    int moj_kierunek = rand() % 4;

    // 1. LĄDOWANIE
    sem_p(SEM_PAS);
    int moj_pas = -1;
    int start_node = rand() % RUNWAYS_COUNT;

    for(int i=0; i<RUNWAYS_COUNT; i++) {
        int idx = (start_node + i) % RUNWAYS_COUNT;
        if (shared_memory->pasy_startowe[idx] == 0) {
            shared_memory->pasy_startowe[idx] = id;
            moj_pas = idx;
            break;
        }
    }
    if (moj_pas == -1) {
        for(int i=0; i<RUNWAYS_COUNT; i++) if(shared_memory->pasy_startowe[i]==0) { shared_memory->pasy_startowe[i]=id; moj_pas=i; break; }
    }

    usleep(LANDING_TIME);

    // 2. PARKOWANIE
    sem_p(SEM_GATE);
    if(moj_pas != -1) shared_memory->pasy_startowe[moj_pas] = 0;
    sem_v(SEM_PAS);

    int my_gate_index = -1;
    start_node = rand() % GATES_COUNT;

    for(int i=0; i<GATES_COUNT; i++) {
        int idx = (start_node + i) % GATES_COUNT;
        if(shared_memory->stanowiska_gate[idx] == 0) {
            shared_memory->stanowiska_gate[idx] = id;
            shared_memory->gate_kierunek[idx] = moj_kierunek;
            shared_memory->gate_liczba_pasazerow[idx] = 0;
            my_gate_index = idx;
            break;
        }
    }
    if (my_gate_index == -1) { sem_v(SEM_GATE); exit(1); }

    // 3. TANKOWANIE
    sem_p(SEM_CYSTERNA);
    int my_tanker_index = -1;
    start_node = rand() % TANKERS_COUNT;

    for(int i=0; i<TANKERS_COUNT; i++) {
        int idx = (start_node + i) % TANKERS_COUNT;
        if(shared_memory->cysterny_status[idx] == 0) {
            shared_memory->cysterny_status[idx] = id;
            my_tanker_index = idx;
            break;
        }
    }

    sem_p(MUTEX_ZASOBY);
    while (shared_memory->paliwo_w_magazynie < FUEL_NEEDED) {
        sem_v(MUTEX_ZASOBY);
        sleep(1);
        sem_p(MUTEX_ZASOBY);
    }
    shared_memory->paliwo_w_magazynie -= FUEL_NEEDED;
    sem_v(MUTEX_ZASOBY);

    sleep(TANKING_TIME);
    if (my_tanker_index != -1) shared_memory->cysterny_status[my_tanker_index] = 0;
    sem_v(SEM_CYSTERNA);

    // 4. BOARDING
    int czas_czekania = 0;
    while (czas_czekania < BOARDING_TIME) {
        sem_p(MUTEX_ZASOBY);
        if (shared_memory->pasazerowie_w_terminalu[moj_kierunek] > 0 &&
            shared_memory->gate_liczba_pasazerow[my_gate_index] < PLANE_CAPACITY) {

            int do_wziecia = BOARDING_SPEED;
            if (do_wziecia > shared_memory->pasazerowie_w_terminalu[moj_kierunek])
                do_wziecia = shared_memory->pasazerowie_w_terminalu[moj_kierunek];
            if (shared_memory->gate_liczba_pasazerow[my_gate_index] + do_wziecia > PLANE_CAPACITY)
                do_wziecia = PLANE_CAPACITY - shared_memory->gate_liczba_pasazerow[my_gate_index];

            shared_memory->pasazerowie_w_terminalu[moj_kierunek] -= do_wziecia;
            shared_memory->gate_liczba_pasazerow[my_gate_index] += do_wziecia;
        }
        bool pelny = (shared_memory->gate_liczba_pasazerow[my_gate_index] >= PLANE_CAPACITY);
        sem_v(MUTEX_ZASOBY);

        if (pelny) break;
        sleep(1);
        czas_czekania++;
    }

    int pas = shared_memory->gate_liczba_pasazerow[my_gate_index];
    const char* status = (pas >= PLANE_CAPACITY) ? "PELNY" : "TIMEOUT";
    dodaj_log("ID:%02d [%c] Pax: %d/%d (%s)", id, KIERUNKI_NAZWY[moj_kierunek], pas, status);

    // 5. ODLOT
    if (my_gate_index != -1) {
        shared_memory->stanowiska_gate[my_gate_index] = 0;
        shared_memory->gate_kierunek[my_gate_index] = -1;
        shared_memory->gate_liczba_pasazerow[my_gate_index] = 0;
    }
    sem_v(SEM_GATE);

    sem_p(SEM_PAS);
    moj_pas = -1;
    start_node = rand() % RUNWAYS_COUNT;

    for(int i=0; i<RUNWAYS_COUNT; i++) {
        int idx = (start_node + i) % RUNWAYS_COUNT;
        if (shared_memory->pasy_startowe[idx] == 0) {
            shared_memory->pasy_startowe[idx] = -id;
            moj_pas = idx;
            break;
        }
    }
    if (moj_pas == -1) {
        for(int i=0; i<RUNWAYS_COUNT; i++) if(shared_memory->pasy_startowe[i]==0) { shared_memory->pasy_startowe[i] = -id; moj_pas=i; break; }
    }

    usleep(LANDING_TIME);

    if(moj_pas != -1) shared_memory->pasy_startowe[moj_pas] = 0;
    sem_v(SEM_PAS);

    exit(0);
}

// --- RYSOWANIE GUI ---
void draw_interface(int loop_counter, int plane_id_counter) {
    erase();
    // RAMKI
    int height = LINES;
    int width = 80;
    mvvline(0, width, ACS_VLINE, height);
    for(int i=0; i<width; i++) { mvaddch(0, i, ACS_HLINE); mvaddch(height-1, i, ACS_HLINE); }
    for(int i=0; i<height; i++) { mvaddch(i, 0, ACS_VLINE); }
    mvaddch(0, 0, ACS_ULCORNER); mvaddch(height-1, 0, ACS_LLCORNER);
    mvaddch(0, width, ACS_TTEE);    mvaddch(height-1, width, ACS_BTEE);

    time_t now = time(NULL);
    int seconds_left = (int)(shared_memory->nastepna_dostawa - now);
    if (seconds_left < 0) seconds_left = 0;

    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(1, 2, " SYMULACJA LOTNISKA - WIZUALIZACJA");
    mvprintw(1, 55, " Dostawa za: %ds ", seconds_left);
    attroff(A_BOLD | COLOR_PAIR(3));
    mvhline(2, 1, ACS_HLINE, width-1);

    // PALIWO
    mvprintw(3, 2, "PALIWO:");
    int bar_width = 30;
    float fuel_ratio = (float)shared_memory->paliwo_w_magazynie / FUEL_MAX;
    int filled_len = (int)(fuel_ratio * bar_width);
    if (filled_len < 0) filled_len = 0; if (filled_len > bar_width) filled_len = bar_width;

    mvprintw(3, 10, "[");
    if (fuel_ratio < 0.2) attron(COLOR_PAIR(2)); else attron(COLOR_PAIR(1));
    for (int i = 0; i < bar_width; i++) {
        if (i < filled_len) addch(ACS_CKBOARD); else addch(' ');
    }
    attroff(COLOR_PAIR(1) | COLOR_PAIR(2));
    printw("] %d/%dL", shared_memory->paliwo_w_magazynie, FUEL_MAX);

    // PASY
    for(int i=0; i<RUNWAYS_COUNT; i++) {
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

    // BRAMKI I CYSTERNY
    attron(A_BOLD);
    mvprintw(8, 2, "BRAMKI (BOARDING):");
    mvprintw(8, 45, "CYSTERNY:");
    attroff(A_BOLD);

    for (int i = 0; i < GATES_COUNT; ++i) {
        int pid = shared_memory->stanowiska_gate[i];
        int kier = shared_memory->gate_kierunek[i];
        int pas = shared_memory->gate_liczba_pasazerow[i];

        mvprintw(9 + i, 2, "G%d: ", i + 1);
        if (pid == 0) {
            attron(COLOR_PAIR(1)); printw("[ .................... ]"); attroff(COLOR_PAIR(1));
        } else {
            if (pas >= PLANE_CAPACITY) attron(COLOR_PAIR(1)); else attron(COLOR_PAIR(3));
            printw("[ ID:%-2d ", pid);
            attron(A_BOLD); printw("%c", KIERUNKI_NAZWY[kier]); attroff(A_BOLD);
            printw(" %2d/%-2d ]", pas, PLANE_CAPACITY);
            if (pas >= PLANE_CAPACITY) attroff(COLOR_PAIR(1)); else attroff(COLOR_PAIR(3));
        }
    }

    for (int i = 0; i < TANKERS_COUNT; ++i) {
        int pid = shared_memory->cysterny_status[i];
        mvprintw(9 + i, 45, "C%d: ", i + 1);
        if (pid == 0) {
            attron(COLOR_PAIR(1)); printw("[ WOLNA ]"); attroff(COLOR_PAIR(1));
        } else {
            attron(COLOR_PAIR(4)); printw("[ TANKUJE ID:%d ]", pid); attroff(COLOR_PAIR(4));
        }
    }

    // --- TERMINAL POD BRAMKAMI ---
    int terminal_y = 9 + GATES_COUNT + 1;
    mvhline(terminal_y - 1, 1, ACS_HLINE, width-1);
    mvprintw(terminal_y, 2, "TERMINAL (OCZEKUJACY NA LOT):");

    int x_pos = 2;
    for(int i=0; i<4; i++) {
        mvprintw(terminal_y + 1, x_pos, "BRAMA %c: ", KIERUNKI_NAZWY[i]);
        attron(COLOR_PAIR(4) | A_BOLD);
        printw("%-3d os.", shared_memory->pasazerowie_w_terminalu[i]);
        attroff(COLOR_PAIR(4) | A_BOLD);
        x_pos += 18;
    }

    // --- GRAFIKI ASCII (WIEŻA + SAMOLOT Z BOKU) ---
    int art_y = terminal_y + 3;

    // Budynek Lotniska (Wieża)
    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(art_y,     5, "      _H_      [ WIEZA KONTROLI ]");
    mvprintw(art_y + 1, 5, "     /__ \\     ");
    mvprintw(art_y + 2, 5, "   __|[_]|__   System Operacyjny");
    mvprintw(art_y + 3, 5, "   | |___| |   Status: ONLINE");
    attroff(A_BOLD | COLOR_PAIR(3));

    // Samolot (Boeing 737 - Widok z boku, pod wieżą)
    int plane_y = art_y + 5; // Samolot niżej
    attron(COLOR_PAIR(4));
    // Prosty, czytelny samolot pasażerski z boku
    mvprintw(plane_y,     5, "         __|__");
    mvprintw(plane_y + 1, 5, "__________(_)__________");
    mvprintw(plane_y + 2, 5, "   O   O       O   O");
    attroff(COLOR_PAIR(4));

    // --- LEGENDA PARAMETRÓW (NA DOLE) ---
    int legend_y = height - 5;
    mvhline(legend_y, 1, ACS_HLINE, width-1);
    attron(A_REVERSE);
    mvprintw(legend_y + 1, 2, " LEGENDA KONFIGURACJI: ");
    attroff(A_REVERSE);

    mvprintw(legend_y + 2, 2, "Spawn Samolotu: co %.1fs | Spawn Pasazerow: %d%% szans", (float)SPAWN_RATE/10.0, PASSENGER_RATE);
    mvprintw(legend_y + 3, 2, "Ladowanie: %.1fs | Tankowanie: %ds | Boarding: %ds", (float)LANDING_TIME/1000000.0, TANKING_TIME, BOARDING_TIME);

    // --- STATYSTYKI ---
    attron(A_BOLD);
    mvprintw(height - 1, 2, "Ilosc samolotow: %d", plane_id_counter - 1);
    attroff(A_BOLD);

    // --- LOGI (PRAWA STRONA - ODLOTY) ---
    int log_x_start = width + 2;
    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(1, log_x_start, "ODLOTY / DEPARTURES:"); // ZMIANA NAZWY
    attroff(A_BOLD | COLOR_PAIR(3));

    for(int i=0; i<LOG_HISTORY_SIZE; i++) {
        int idx = (shared_memory->log_index - 1 - i + LOG_HISTORY_SIZE) % LOG_HISTORY_SIZE;
        char* linia = shared_memory->historia_logow[idx];

        if (strlen(linia) > 0) {
            if (strstr(linia, "PELNY")) attron(COLOR_PAIR(1));
            else attron(COLOR_PAIR(4));

            mvprintw(3 + i, log_x_start, "%s", linia);

            if (strstr(linia, "PELNY")) attroff(COLOR_PAIR(1));
            else attroff(COLOR_PAIR(4));
        }
    }

    refresh();
}

int main() {
    srand(time(NULL));

    // Reset pamięci (Standardowa procedura)
    int old_shmid = shmget(SHM_KEY, sizeof(SharedData), 0666);
    if (old_shmid != -1) shmctl(old_shmid, IPC_RMID, nullptr);
    int old_semid = semget(SEM_KEY, 4, 0666);
    if (old_semid != -1) semctl(old_semid, 0, IPC_RMID);

    shmid = shmget(SHM_KEY, sizeof(SharedData), IPC_CREAT | 0666);
    shared_memory = (SharedData*)shmat(shmid, nullptr, 0);
    memset(shared_memory, 0, sizeof(SharedData));

    shared_memory->paliwo_w_magazynie = FUEL_MAX;
    shared_memory->nastepna_dostawa = time(NULL) + DELIVERY_TIME;

    semid = semget(SEM_KEY, 4, IPC_CREAT | 0666);
    semctl(semid, SEM_PAS, SETVAL, RUNWAYS_COUNT);
    semctl(semid, SEM_GATE, SETVAL, GATES_COUNT);
    semctl(semid, SEM_CYSTERNA, SETVAL, TANKERS_COUNT);
    semctl(semid, MUTEX_ZASOBY, SETVAL, 1);

    signal(SIGINT, cleanup);

    if (fork() == 0) { proces_dostawcy_paliwa(); exit(0); }

    initscr();
    noecho();
    curs_set(0);
    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_RED, COLOR_BLACK);
    init_pair(3, COLOR_CYAN, COLOR_BLACK);
    init_pair(4, COLOR_YELLOW, COLOR_BLACK);

    // Obsługa PAUZY (Spacja)
    nodelay(stdscr, TRUE);

    int loop_counter = 0;
    int plane_id_counter = 1;
    bool paused = false;

    while (true) {
        int ch = getch();
        if (ch == ' ') {
            paused = !paused;
            if (paused) {
                attron(COLOR_PAIR(2) | A_BOLD | A_BLINK);
                mvprintw(LINES/2, 40 - 15, " !!! PAUZA - SPACJA ABY WZNOWIC !!! ");
                attroff(COLOR_PAIR(2) | A_BOLD | A_BLINK);
                refresh();
                nodelay(stdscr, FALSE);
                while(getch() != ' ');
                nodelay(stdscr, TRUE);
                paused = false;
            }
        }

        if (!paused) {
            draw_interface(loop_counter, plane_id_counter);

            if (loop_counter % SPAWN_RATE == 0) {
                if (fork() == 0) {
                    proces_samolotu(plane_id_counter);
                } else {
                    plane_id_counter++;
                }
            }

            // Balans pasażerów
            if (rand() % 100 < PASSENGER_RATE) {
                sem_p(MUTEX_ZASOBY);
                int kier = rand() % 4;
                shared_memory->pasazerowie_w_terminalu[kier] += (1 + rand() % PASSENGER_GROUP_SIZE);
                sem_v(MUTEX_ZASOBY);
            }

            loop_counter++;
        }

        while (waitpid(-1, NULL, WNOHANG) > 0);
        usleep(100000);
    }
    return 0;
}