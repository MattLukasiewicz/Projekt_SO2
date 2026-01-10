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

// =============================================================
// ============  SEKCJA KONFIGURACJI (WYBIERZ SCENARIUSZ) ======
// =============================================================

// Odkomentuj JEDEN blok, resztę zakomentuj (/* ... */)

// --- SCENARIUSZ 1: WĄSKIE GARDŁO (BRAMKI) ---
// Cel: Pokazać korek na pasie, bo nie ma gdzie zaparkować.

// --- SCENARIUSZ 1: PRAWDZIWY KOREK NA PASIE ---

#define RUNWAYS_COUNT 2
#define GATES_COUNT 3        // Mało bramek
#define TANKERS_COUNT 3
#define FUEL_MAX 10000
#define FUEL_NEEDED 500
#define FUEL_DELIVERY 5000
#define DELIVERY_TIME 10
#define LANDING_TIME 500000  // 0.5s (Szybkie lądowanie)
#define TANKING_TIME 5       // ZMIANA: 5s (Długi postój - zatyka bramki!)
#define SPAWN_RATE 10        // ZMIANA: Szybki generator (co 1.0s)


// --- SCENARIUSZ 2: KRYZYS PALIWOWY ---
// Cel: Lotnisko staje, bo brakuje paliwa (zasób odnawialny).
/*
#define RUNWAYS_COUNT 2
#define GATES_COUNT 10       // Dużo bramek
#define TANKERS_COUNT 10     // Dużo cystern
#define FUEL_MAX 3000        // Mały magazyn!
#define FUEL_NEEDED 800      // Pazerne samoloty
#define FUEL_DELIVERY 2000   // Mała dostawa
#define DELIVERY_TIME 25     // Bardzo rzadka dostawa (KRYZYS!)
#define LANDING_TIME 1000000
#define TANKING_TIME 2
#define SPAWN_RATE 20
*/

// --- SCENARIUSZ 3: AWARIA PASA (STARE LOTNISKO) ---
// Cel: Bramki puste, bo pasy nie wyrabiają.
/*
#define RUNWAYS_COUNT 1      // Tylko jeden pas (KOREK TUTAJ!)
#define GATES_COUNT 10       // Puste bramki
#define TANKERS_COUNT 5      // Puste cysterny
#define FUEL_MAX 10000
#define FUEL_NEEDED 500
#define FUEL_DELIVERY 5000
#define DELIVERY_TIME 15
#define LANDING_TIME 4000000 // 4s (Bardzo wolne lądowanie/awaria)
#define TANKING_TIME 2
#define SPAWN_RATE 15        // Szybki generator

*/
// --- SCENARIUSZ 4: IDEALNA HARMONIA (DEFAULT) ---
/*
#define RUNWAYS_COUNT 2
#define GATES_COUNT 8
#define TANKERS_COUNT 4
#define FUEL_MAX 10000
#define FUEL_NEEDED 600
#define FUEL_DELIVERY 4000
#define DELIVERY_TIME 12
#define LANDING_TIME 1500000 // 1.5s
#define TANKING_TIME 3       // 3s
#define SPAWN_RATE 25        // Zbalansowany ruch (co 2.5s)
*/

// =============================================================
// ==================  KONIEC KONFIGURACJI  ====================
// =============================================================

#define SHM_KEY 12345
#define SEM_KEY 54321

// --- INDEKSY SEMAFORÓW ---
#define SEM_PAS 0
#define SEM_GATE 1
#define SEM_CYSTERNA 2
#define MUTEX_PALIWO 3

// --- STRUKTURA DANYCH ---
struct SharedData {
    int paliwo_w_magazynie;
    int ilosc_cystern;
    time_t nastepna_dostawa;

    // Tablice do wizualizacji
    int pasy_startowe[RUNWAYS_COUNT];
    int stanowiska_gate[GATES_COUNT];
    int cysterny_status[TANKERS_COUNT];
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

// --- CLEANUP ---
void cleanup(int signum) {
    endwin();
    // Usuwamy zasoby tylko jeśli jesteśmy procesem głównym lub sprzątamy po Ctrl+C
    if (shared_memory != nullptr) shmdt(shared_memory);
    if (shmid != -1) shmctl(shmid, IPC_RMID, nullptr);
    if (semid != -1) semctl(semid, 0, IPC_RMID);

    // Zabijamy wszystkie dzieci (samoloty)
    kill(0, SIGKILL);
    exit(0);
}

// --- PROCES DOSTAWCY ---
void proces_dostawcy_paliwa() {
    while(true) {
        shared_memory->nastepna_dostawa = time(NULL) + DELIVERY_TIME;
        sleep(DELIVERY_TIME);

        sem_p(MUTEX_PALIWO);
        shared_memory->paliwo_w_magazynie += FUEL_DELIVERY;
        if (shared_memory->paliwo_w_magazynie > FUEL_MAX) {
            shared_memory->paliwo_w_magazynie = FUEL_MAX;
        }
        sem_v(MUTEX_PALIWO);
    }
}

// --- LOGIKA SAMOLOTU ---
void proces_samolotu(int id) {
    srand(getpid() + id);

    // 1. LĄDOWANIE
    sem_p(SEM_PAS);

    int moj_pas = -1;
    for(int i=0; i<RUNWAYS_COUNT; i++) {
        if (shared_memory->pasy_startowe[i] == 0) {
            shared_memory->pasy_startowe[i] = id;
            moj_pas = i;
            break;
        }
    }

    usleep(LANDING_TIME);

    // --- KLUCZOWA ZMIANA: BLOKADA PASA ---
    // Samolot nie zjedzie z pasa, dopóki nie dostanie miejsca w bramce
    sem_p(SEM_GATE);

    // Mamy bramkę, więc zwalniamy pas
    if(moj_pas != -1) shared_memory->pasy_startowe[moj_pas] = 0;
    sem_v(SEM_PAS);

    // 2. PARKOWANIE (GATE) - semafor już mamy zajęty wyżej
    int my_gate_index = -1;
    for(int i=0; i<GATES_COUNT; i++) {
        if(shared_memory->stanowiska_gate[i] == 0) {
            shared_memory->stanowiska_gate[i] = id;
            my_gate_index = i;
            break;
        }
    }

    if (my_gate_index == -1) {
        sem_v(SEM_GATE);
        exit(1);
    }

    // 3. TANKOWANIE
    sem_p(SEM_CYSTERNA);
    int my_tanker_index = -1;
    for(int i=0; i<TANKERS_COUNT; i++) {
        if(shared_memory->cysterny_status[i] == 0) {
            shared_memory->cysterny_status[i] = id;
            my_tanker_index = i;
            break;
        }
    }

    sem_p(MUTEX_PALIWO);
    while (shared_memory->paliwo_w_magazynie < FUEL_NEEDED) {
        sem_v(MUTEX_PALIWO);
        sleep(1);
        sem_p(MUTEX_PALIWO);
    }
    shared_memory->paliwo_w_magazynie -= FUEL_NEEDED;
    sem_v(MUTEX_PALIWO);

    sleep(TANKING_TIME);

    if (my_tanker_index != -1) shared_memory->cysterny_status[my_tanker_index] = 0;
    sem_v(SEM_CYSTERNA);

    // 4. START
    if (my_gate_index != -1) shared_memory->stanowiska_gate[my_gate_index] = 0;
    sem_v(SEM_GATE);

    sem_p(SEM_PAS);

    moj_pas = -1;
    for(int i=0; i<RUNWAYS_COUNT; i++) {
        if (shared_memory->pasy_startowe[i] == 0) {
            shared_memory->pasy_startowe[i] = id;
            moj_pas = i;
            break;
        }
    }

    usleep(LANDING_TIME);

    if(moj_pas != -1) shared_memory->pasy_startowe[moj_pas] = 0;
    sem_v(SEM_PAS);

    exit(0);
}

// --- RYSOWANIE GUI ---
void draw_interface(int loop_counter, int plane_id_counter) {
    erase();
    box(stdscr, 0, 0);

    time_t now = time(NULL);
    int seconds_left = (int)(shared_memory->nastepna_dostawa - now);
    if (seconds_left < 0) seconds_left = 0;

    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(1, 2, " SYMULACJA LOTNISKA - %d PASY STARTOWE ", RUNWAYS_COUNT);
    mvprintw(1, 70, " Dostawa: %ds ", seconds_left);
    attroff(A_BOLD | COLOR_PAIR(3));
    mvhline(2, 1, ACS_HLINE, 88);

    // PALIWO
    mvprintw(4, 2, "MAGAZYN PALIWA:");
    int max_bar = 40;
    long current_bar = ((long)shared_memory->paliwo_w_magazynie * max_bar) / FUEL_MAX;
    if(current_bar > max_bar) current_bar = max_bar;

    mvprintw(4, 18, "[");
    mvprintw(4, 18 + max_bar + 1, "]");

    if (current_bar < 5) attron(COLOR_PAIR(2) | A_BLINK);
    else if (current_bar < 10) attron(COLOR_PAIR(2));
    else attron(COLOR_PAIR(1));

    for (int i = 0; i < max_bar; i++) {
        if (i < current_bar) mvaddch(4, 19 + i, ACS_CKBOARD);
        else mvaddch(4, 19 + i, ' ');
    }

    if (current_bar < 5) attroff(COLOR_PAIR(2) | A_BLINK);
    else if (current_bar < 10) attroff(COLOR_PAIR(2));
    else attroff(COLOR_PAIR(1));
    mvprintw(4, 62, "%5d / %d L", shared_memory->paliwo_w_magazynie, FUEL_MAX);

    // PASY
    mvhline(5, 1, ACS_HLINE, 88);
    int y_offset = 6;
    for(int i=0; i<RUNWAYS_COUNT; i++) {
        mvprintw(y_offset + i, 2, "PAS %d:", i + 1);
        int pid = shared_memory->pasy_startowe[i];

        if (pid == 0) {
            attron(COLOR_PAIR(1) | A_REVERSE);
            mvprintw(y_offset + i, 10, " [ WOLNY ]       ");
            attroff(COLOR_PAIR(1) | A_REVERSE);
        } else {
            attron(COLOR_PAIR(2) | A_REVERSE);
            mvprintw(y_offset + i, 10, " [ ZAJETY: ID %d ] ", pid);
            attroff(COLOR_PAIR(2) | A_REVERSE);
        }
    }

    y_offset += RUNWAYS_COUNT + 1;
    mvhline(y_offset - 1, 1, ACS_HLINE, 88);

    // GATE I CYSTERNY
    attron(A_BOLD);
    mvprintw(y_offset, 5, "BRAMKI (GATES):");
    mvprintw(y_offset, 50, "CYSTERNY:");
    attroff(A_BOLD);
    y_offset++;

    int max_rows = (GATES_COUNT > TANKERS_COUNT) ? GATES_COUNT : TANKERS_COUNT;

    for (int i = 0; i < max_rows; ++i) {
        if (i < GATES_COUNT) {
            int pid = shared_memory->stanowiska_gate[i];
            mvprintw(y_offset + i, 5, "G%d:", i + 1);
            if (pid == 0) {
                attron(COLOR_PAIR(1)); printw(" [ ... ] "); attroff(COLOR_PAIR(1));
            } else {
                attron(COLOR_PAIR(3)); printw(" [%-5d] ", pid); attroff(COLOR_PAIR(3));
            }
        }

        if (i < TANKERS_COUNT) {
            int pid = shared_memory->cysterny_status[i];
            mvprintw(y_offset + i, 50, "C%d:", i + 1);
            if (pid == 0) {
                attron(COLOR_PAIR(1)); printw(" [ WOLNA ] "); attroff(COLOR_PAIR(1));
            } else {
                attron(COLOR_PAIR(4) | A_BOLD);
                printw(" [TANKUJE %d]", pid);
                attroff(COLOR_PAIR(4) | A_BOLD);
            }
        }
    }

    mvprintw(LINES - 2, 2, "Liczba samolotow (Total): %d | SPAWN: co %.1fs", plane_id_counter - 1, (float)SPAWN_RATE/10.0);
    refresh();
}

int main() {
    // --- AUTOMATYCZNE CZYSZCZENIE PRZY STARCIE ---
    // Próbujemy usunąć starą pamięć, jeśli istnieje
    int old_shmid = shmget(SHM_KEY, sizeof(SharedData), 0666);
    if (old_shmid != -1) shmctl(old_shmid, IPC_RMID, nullptr);

    int old_semid = semget(SEM_KEY, 4, 0666);
    if (old_semid != -1) semctl(old_semid, 0, IPC_RMID);
    // ----------------------------------------------

    // 1. Inicjalizacja Pamięci
    shmid = shmget(SHM_KEY, sizeof(SharedData), IPC_CREAT | 0666);
    shared_memory = (SharedData*)shmat(shmid, nullptr, 0);

    // Zerowanie pamięci (reset danych)
    memset(shared_memory, 0, sizeof(SharedData));

    // Ustawienie wartości początkowych
    shared_memory->paliwo_w_magazynie = FUEL_MAX;
    shared_memory->ilosc_cystern = TANKERS_COUNT;
    shared_memory->nastepna_dostawa = time(NULL) + DELIVERY_TIME;

    // 2. Semafory
    semid = semget(SEM_KEY, 4, IPC_CREAT | 0666);

    semctl(semid, SEM_PAS, SETVAL, RUNWAYS_COUNT);
    semctl(semid, SEM_GATE, SETVAL, GATES_COUNT);
    semctl(semid, SEM_CYSTERNA, SETVAL, TANKERS_COUNT);
    semctl(semid, MUTEX_PALIWO, SETVAL, 1);

    signal(SIGINT, cleanup);

    // 3. START DOSTAWCY
    if (fork() == 0) {
        proces_dostawcy_paliwa();
        exit(0);
    }

    // 4. GUI
    initscr();
    noecho();
    curs_set(0);
    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_RED, COLOR_BLACK);
    init_pair(3, COLOR_CYAN, COLOR_BLACK);
    init_pair(4, COLOR_YELLOW, COLOR_BLACK);

    int loop_counter = 0;
    int plane_id_counter = 1;

    // 5. Główna Pętla
    while (true) {
        draw_interface(loop_counter, plane_id_counter);

        if (loop_counter % SPAWN_RATE == 0) {
            if (fork() == 0) {
                proces_samolotu(plane_id_counter);
            } else {
                plane_id_counter++;
            }
        }
        loop_counter++;

        while (waitpid(-1, NULL, WNOHANG) > 0);
        usleep(100000); // 0.1s tick
    }
    return 0;
}

