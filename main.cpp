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

// --- KONFIGURACJA ---
#define SHM_KEY 12345
#define SEM_KEY 54321
#define FUEL_MAX 5000
#define GATES_COUNT 5
#define TANKERS_COUNT 3
#define FUEL_NEEDED 500
#define FUEL_DELIVERY 3000
#define DELIVERY_TIME 15

// --- INDEKSY SEMAFORÓW ---
#define SEM_PAS 0
#define SEM_GATE 1
#define SEM_CYSTERNA 2
#define MUTEX_PALIWO 3

// --- STRUKTURA DANYCH (ZMIANA!) ---
struct SharedData {
    int paliwo_w_magazynie;
    int ilosc_cystern;
    bool pas_startowy_wolny;
    int id_samolotu_na_pasie;
    int stanowiska_gate[GATES_COUNT];
    int cysterny_status[TANKERS_COUNT];

    // NOWE POLE: Jedno źródło prawdy o czasie dostawy
    time_t nastepna_dostawa;
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
    if (shared_memory != nullptr) shmdt(shared_memory);
    if (shmid != -1) shmctl(shmid, IPC_RMID, nullptr);
    if (semid != -1) semctl(semid, 0, IPC_RMID);
    kill(0, SIGKILL);
    exit(0);
}

// --- PROCES DOSTAWCY (POPRAWIONY) ---
void proces_dostawcy_paliwa() {
    while(true) {
        // 1. Ustal kiedy będzie następna dostawa (Teraz + 15s)
        // Zapisujemy to w pamięci dzielonej, żeby wizualizacja wiedziała DOKŁADNIE to samo
        shared_memory->nastepna_dostawa = time(NULL) + DELIVERY_TIME;

        // 2. Idź spać
        sleep(DELIVERY_TIME);

        // 3. Dostawa paliwa (po obudzeniu)
        sem_p(MUTEX_PALIWO);
        shared_memory->paliwo_w_magazynie += FUEL_DELIVERY;
        if (shared_memory->paliwo_w_magazynie > FUEL_MAX) {
            shared_memory->paliwo_w_magazynie = FUEL_MAX;
        }
        sem_v(MUTEX_PALIWO);
    }
}

// --- LOGIKA SAMOLOTU (BEZ ZMIAN) ---
void proces_samolotu(int id) {
    srand(getpid() + id);

    // Lądowanie
    sem_p(SEM_PAS);
    shared_memory->pas_startowy_wolny = false;
    shared_memory->id_samolotu_na_pasie = id;
    sleep(2);
    shared_memory->pas_startowy_wolny = true;
    shared_memory->id_samolotu_na_pasie = 0;
    sem_v(SEM_PAS);

    // Gate
    sem_p(SEM_GATE);
    int my_gate_index = -1;
    for(int i=0; i<GATES_COUNT; i++) {
        if(shared_memory->stanowiska_gate[i] == 0) {
            shared_memory->stanowiska_gate[i] = id;
            my_gate_index = i;
            break;
        }
    }

    // Tankowanie
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

    sleep(2);

    if (my_tanker_index != -1) shared_memory->cysterny_status[my_tanker_index] = 0;
    sem_v(SEM_CYSTERNA);

    if (my_gate_index != -1) shared_memory->stanowiska_gate[my_gate_index] = 0;
    sem_v(SEM_GATE);

    exit(0);
}

// --- RYSOWANIE GUI (POPRAWIONE OBLICZANIE CZASU) ---
void draw_interface(int loop_counter, int plane_id_counter) {
    erase();
    box(stdscr, 0, 0);

    // Obliczanie czasu dostawy na podstawie PAMIĘCI DZIELONEJ (Wspólna Prawda)
    time_t now = time(NULL);
    int seconds_left = (int)(shared_memory->nastepna_dostawa - now);
    if (seconds_left < 0) seconds_left = 0; // Zabezpieczenie wizualne

    attron(A_BOLD | COLOR_PAIR(3));
    mvprintw(1, 2, " SYSTEM KONTROLI LOTNISKA v3.0 (PERFECT SYNC) ");
    mvprintw(1, 50, " Czas symulacji: %ds ", loop_counter / 10);

    // Wyświetlamy dokładnie to, co zaplanował dostawca
    mvprintw(1, 70, " Dostawa: %ds ", seconds_left);
    attroff(A_BOLD | COLOR_PAIR(3));
    mvhline(2, 1, ACS_HLINE, 88); // Poszerzyłem linię

    // Reszta rysowania bez zmian...
    mvprintw(4, 2, "MAGAZYN PALIWA:");
    int max_bar = 40;
    int current_bar = (shared_memory->paliwo_w_magazynie * max_bar) / FUEL_MAX;

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
    mvprintw(4, 62, "%4d / %d L", shared_memory->paliwo_w_magazynie, FUEL_MAX);

    mvhline(6, 1, ACS_HLINE, 88);
    if (shared_memory->pas_startowy_wolny) {
        attron(COLOR_PAIR(1) | A_REVERSE);
        mvprintw(7, 35, " [ PAS WOLNY ] ");
        attroff(COLOR_PAIR(1) | A_REVERSE);
    } else {
        attron(COLOR_PAIR(2) | A_REVERSE);
        mvprintw(7, 30, " [ LADUJE SAMOLOT ID: %d ] ", shared_memory->id_samolotu_na_pasie);
        attroff(COLOR_PAIR(2) | A_REVERSE);
    }
    mvhline(8, 1, ACS_HLINE, 88);

    attron(A_BOLD);
    mvprintw(9, 5, "BRAMKI (GATES):");
    mvprintw(9, 50, "CYSTERNY (TANKOWANIE):");
    attroff(A_BOLD);

    for (int i = 0; i < GATES_COUNT; ++i) {
        int pid = shared_memory->stanowiska_gate[i];
        mvprintw(11 + i, 5, "GATE %d:", i + 1);
        if (pid == 0) {
            attron(COLOR_PAIR(1)); printw(" [ WOLNY ] "); attroff(COLOR_PAIR(1));
        } else {
            attron(COLOR_PAIR(3)); printw(" [ ID: %-3d ] ", pid); attroff(COLOR_PAIR(3));
        }
    }

    for (int i = 0; i < TANKERS_COUNT; ++i) {
        int pid_obslugiwany = shared_memory->cysterny_status[i];
        mvprintw(11 + i, 50, "CYSTERNA %d:", i + 1);
        if (pid_obslugiwany == 0) {
            attron(COLOR_PAIR(1)); printw(" [ CZEKA ] "); attroff(COLOR_PAIR(1));
        } else {
            attron(COLOR_PAIR(4) | A_BOLD);
            printw(" [ TANKUJE ID: %d ] ", pid_obslugiwany);
            attroff(COLOR_PAIR(4) | A_BOLD);
        }
    }

    mvhline(22, 1, ACS_HLINE, 88);
    mvprintw(23, 2, "Samoloty: %d | Ctrl+C = Koniec", plane_id_counter - 1);
    refresh();
}

int main() {
    // 1. Inicjalizacja Pamięci
    shmid = shmget(SHM_KEY, sizeof(SharedData), IPC_CREAT | 0666);
    shared_memory = (SharedData*)shmat(shmid, nullptr, 0);

    // Inicjalizacja Wartości
    shared_memory->paliwo_w_magazynie = FUEL_MAX;
    shared_memory->ilosc_cystern = TANKERS_COUNT;
    shared_memory->pas_startowy_wolny = true;
    // Inicjalizujemy czas pierwszej dostawy na "zaraz + 15s"
    shared_memory->nastepna_dostawa = time(NULL) + DELIVERY_TIME;

    for(int i=0; i<GATES_COUNT; i++) shared_memory->stanowiska_gate[i] = 0;
    for(int i=0; i<TANKERS_COUNT; i++) shared_memory->cysterny_status[i] = 0;

    // 2. Semafory
    semid = semget(SEM_KEY, 4, IPC_CREAT | 0666);
    semctl(semid, SEM_PAS, SETVAL, 1);
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

    // 5. Pętla
    while (true) {
        draw_interface(loop_counter, plane_id_counter);

        if (loop_counter % 25 == 0) {
            if (fork() == 0) {
                proces_samolotu(plane_id_counter);
            } else {
                plane_id_counter++;
            }
        }
        loop_counter++;

        while (waitpid(-1, NULL, WNOHANG) > 0);
        usleep(100000);
    }
    return 0;
}




/*
cd build        # wejdz do folderu build
rm -rf * # wyczysc stare pliki (dla pewnosci)
cmake ..        # skonfiguruj
make            # zbuduj
./SO2           # uruchom
 *
 */