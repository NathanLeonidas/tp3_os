#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <semaphore.h>

// =====================================================
// CONFIGURATION
// =====================================================

#define BUFFER_SIZE 5
#define N_CAMERAS 3
#define N_SCREENS 2
#define VIDEO_SIZE 64

#define FREE 0
#define READY 1
#define IN_USE 2

#define CMD_BACKUP 1
#define CMD_RESTORE 2
#define CMD_EXIT 3

// =====================================================
// FONCTIONS FOURNIES
// =====================================================

static int video_id = 0;

char *createVideo() {
    char *buf = malloc(64);

    if (!buf) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }

    sprintf(buf, "Video-%d", video_id++);

    return buf;
}

void broadcastVideo(const char *video) {
    printf("[SCREEN] Diffusion: %s\n", video);
}

// =====================================================
// SIGNAL DE PANNE
// =====================================================

volatile sig_atomic_t breakdown_flag = 0;

void handle_breakdown(int sig) {
    (void)sig;
    breakdown_flag = 1;
}

// =====================================================
// COMMUNICATION ENTRE PROCESSUS
// =====================================================

int write_full(int fd, const void *buffer, size_t size) {

    const char *p = buffer;

    while (size > 0) {

        ssize_t n = write(fd, p, size);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        p += n;
        size -= n;
    }

    return 0;
}

int read_full(int fd, void *buffer, size_t size) {

    char *p = buffer;

    while (size > 0) {

        ssize_t n = read(fd, p, size);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        p += n;
        size -= n;
    }

    return 0;
}

// =====================================================
// MONITEUR DE HOARE
// =====================================================

typedef struct {
    sem_t semaphore;
    int waiting;
} HoareCondition;

typedef struct {
    sem_t mutex;
    sem_t urgent;
    int urgent_count;

    HoareCondition not_empty;
    HoareCondition not_full;

} HoareMonitor;


// Attendre sur un sémaphore.
void semaphore_wait(sem_t *s) {

    while (sem_wait(s) == -1) {

        if (errno != EINTR) {
            perror("sem_wait");
            exit(EXIT_FAILURE);
        }
    }
}


// Initialiser le moniteur.
void monitor_init(HoareMonitor *m) {

    sem_init(&m->mutex, 0, 1);
    sem_init(&m->urgent, 0, 0);

    m->urgent_count = 0;

    sem_init(&m->not_empty.semaphore, 0, 0);
    m->not_empty.waiting = 0;

    sem_init(&m->not_full.semaphore, 0, 0);
    m->not_full.waiting = 0;
}


// Entrer dans le moniteur.
void monitor_enter(HoareMonitor *m) {
    semaphore_wait(&m->mutex);
}


// Sortir du moniteur.
void monitor_leave(HoareMonitor *m) {

    if (m->urgent_count > 0)
        sem_post(&m->urgent);
    else
        sem_post(&m->mutex);
}


// Attendre une condition.
void hoare_wait(HoareMonitor *m, HoareCondition *c) {

    c->waiting++;

    if (m->urgent_count > 0)
        sem_post(&m->urgent);
    else
        sem_post(&m->mutex);

    semaphore_wait(&c->semaphore);

    c->waiting--;
}


// Signaler une condition avec transfert immédiat.
void hoare_signal(HoareMonitor *m, HoareCondition *c) {

    if (c->waiting > 0) {

        m->urgent_count++;

        sem_post(&c->semaphore);

        semaphore_wait(&m->urgent);

        m->urgent_count--;
    }
}

// =====================================================
// SERVEUR PRINCIPAL
// =====================================================

typedef struct {

    char video[VIDEO_SIZE];

    int state;

} VideoSlot;


typedef struct {

    VideoSlot slots[BUFFER_SIZE];

    int stopping;

    HoareMonitor monitor;

} Server;


Server server;


// Initialisation.
void server_init() {

    server.stopping = 0;

    for (int i = 0; i < BUFFER_SIZE; i++) {

        server.slots[i].state = FREE;

        server.slots[i].video[0] = '\0';
    }

    monitor_init(&server.monitor);
}


// Chercher une place libre.
int find_free_slot() {

    for (int i = 0; i < BUFFER_SIZE; i++) {

        if (server.slots[i].state == FREE)
            return i;
    }

    return -1;
}


// Chercher une vidéo disponible.
int find_ready_slot() {

    for (int i = 0; i < BUFFER_SIZE; i++) {

        if (server.slots[i].state == READY)
            return i;
    }

    return -1;
}


// =====================================================
// PRODUCTEUR : AJOUTER UNE VIDÉO
// =====================================================

int server_add_video(const char *video) {

    HoareMonitor *m = &server.monitor;

    monitor_enter(m);

    while (find_free_slot() == -1 && !server.stopping) {
        hoare_wait(m, &m->not_full);
    }

    if (server.stopping) {

        monitor_leave(m);

        return -1;
    }

    int index = find_free_slot();

    strcpy(server.slots[index].video, video);

    server.slots[index].state = READY;

    printf("[SERVER] Video ajoutee : %s\n", video);

    hoare_signal(m, &m->not_empty);

    monitor_leave(m);

    return 0;
}


// =====================================================
// CONSOMMATEUR : RÉSERVER UNE VIDÉO
// =====================================================

int server_get_video(char *video) {

    HoareMonitor *m = &server.monitor;

    monitor_enter(m);

    while (find_ready_slot() == -1 && !server.stopping) {
        hoare_wait(m, &m->not_empty);
    }

    if (server.stopping) {

        monitor_leave(m);

        return -1;
    }

    int index = find_ready_slot();

    strcpy(video, server.slots[index].video);

    // La vidéo n'est PAS supprimée.
    // Elle est simplement réservée.

    server.slots[index].state = IN_USE;

    printf("[SERVER] Reservation : %s\n", video);

    monitor_leave(m);

    return index;
}


// =====================================================
// SECTION 2 : VALIDATION DE LA DIFFUSION
// =====================================================

void server_commit_video(int index) {

    HoareMonitor *m = &server.monitor;

    monitor_enter(m);

    if (!breakdown_flag && !server.stopping) {

        server.slots[index].state = FREE;

        server.slots[index].video[0] = '\0';

        printf("[SERVER] Video validee et supprimee.\n");

        hoare_signal(m, &m->not_full);
    }

    monitor_leave(m);
}


// =====================================================
// ARRÊT DU SERVEUR
// =====================================================

void server_stop() {
    HoareMonitor *m = &server.monitor;

    monitor_enter(m);
    server.stopping = 1;

    // Réveiller TOUS les producteurs bloqués sans utiliser hoare_signal
    // (qui causerait un deadlock en bloquant sur le sémaphore 'urgent')
    for (int i = 0; i < m->not_full.waiting; i++) {
        sem_post(&m->not_full.semaphore);
    }

    // Réveiller TOUS les consommateurs bloqués
    for (int i = 0; i < m->not_empty.waiting; i++) {
        sem_post(&m->not_empty.semaphore);
    }

    monitor_leave(m);
}


// =====================================================
// CAMÉRAS
// =====================================================

pthread_mutex_t generation_mutex = PTHREAD_MUTEX_INITIALIZER;


char *safe_create_video() {

    pthread_mutex_lock(&generation_mutex);

    char *video = createVideo();

    pthread_mutex_unlock(&generation_mutex);

    return video;
}


void *camera_thread(void *arg) {

    int id = *(int *)arg;

    while (1) {

        char *video = safe_create_video();

        printf("[CAMERA %d] Production : %s\n", id, video);

        int result = server_add_video(video);

        free(video);

        if (result == -1)
            break;

        usleep(500000);
    }

    printf("[CAMERA %d] Arret.\n", id);

    return NULL;
}


// =====================================================
// ÉCRANS
// =====================================================

void *screen_thread(void *arg) {
    int id = *(int *)arg;

    while (1) {
        char video[VIDEO_SIZE];

        int index = server_get_video(video);

        if (index == -1)
            break;

        // 1. VÉRIFIER LA PANNE AVANT DE COMMENCER LA DIFFUSION
        // Si on est arrêté ici, la vidéo est réservée (IN_USE) mais pas encore diffusée.
        // On ne fait pas de commit, elle sera restaurée en READY pour un autre écran.
        if (breakdown_flag) {
            printf("[SCREEN %d] Panne détectée avant diffusion de %s. Vidéo conservée pour reprise.\n", id, video);
            break; 
        }

        printf("[SCREEN %d] Reception : %s\n", id, video);

        // 2. SIMULATION DE LA DIFFUSION (1,5 seconde)
        usleep(1500000);

        // 3. VÉRIFIER LA PANNE APRÈS LA DIFFUSION
        // Si une panne est survenue PENDANT le usleep, on considère la diffusion comme acquise.
        // On valide la vidéo pour éviter qu'elle ne soit reprise en doublon par un autre écran.
        if (breakdown_flag) {
            printf("[SCREEN %d] Panne survenue PENDANT la diffusion de %s. Validation forcée pour éviter la perte/doublon.\n", id, video);
            server_commit_video(index); // <-- AJOUT CRUCIAL
            break;
        }

        // 4. CAS NORMAL : Diffusion réussie sans panne
        broadcastVideo(video);
        server_commit_video(index);
    }

    printf("[SCREEN %d] Arret.\n", id);
    return NULL;
}

// =====================================================
// SAUVEGARDE ET RESTAURATION
// =====================================================

typedef struct {

    VideoSlot slots[BUFFER_SIZE];

} ServerSnapshot;


// Faire une copie cohérente du serveur.
void server_snapshot(ServerSnapshot *snapshot) {

    HoareMonitor *m = &server.monitor;

    monitor_enter(m);

    for (int i = 0; i < BUFFER_SIZE; i++) {

        snapshot->slots[i] = server.slots[i];

        // Toute vidéo interrompue redevient disponible.
        if (snapshot->slots[i].state == IN_USE) {

            snapshot->slots[i].state = READY;
        }
    }

    monitor_leave(m);
}


// Restaurer le serveur principal.
void server_restore(ServerSnapshot *snapshot) {

    HoareMonitor *m = &server.monitor;

    monitor_enter(m);

    for (int i = 0; i < BUFFER_SIZE; i++) {

        server.slots[i] = snapshot->slots[i];

        if (server.slots[i].state == IN_USE) {
            server.slots[i].state = READY;
        }
    }

    server.stopping = 0;

    monitor_leave(m);

    printf("[SERVER] Restauration terminee.\n");
}


// =====================================================
// SERVEUR SECONDAIRE
// =====================================================

void backup_server(int socket_fd) {

    ServerSnapshot backup;

    printf("[BACKUP] Serveur secondaire demarre.\n");

    while (1) {

        int command;

        if (read_full(socket_fd, &command, sizeof(command)) == -1)
            break;

        // Recevoir une sauvegarde.
        if (command == CMD_BACKUP) {

            printf("[BACKUP] Reception des donnees...\n");

            if (read_full(socket_fd, &backup, sizeof(backup)) == -1)
                break;

            printf("[BACKUP] Copie terminee.\n");

            // Confirmation.
            int ack = 1;

            if (write_full(socket_fd, &ack, sizeof(ack)) == -1)
                break;

            printf("[BACKUP] Confirmation ACK envoyee.\n");
        }

        // Envoyer la sauvegarde au serveur principal.
        else if (command == CMD_RESTORE) {

            printf("[BACKUP] Envoi des donnees au serveur principal.\n");

            if (write_full(socket_fd, &backup, sizeof(backup)) == -1)
                break;

            printf("[BACKUP] Restauration transmise.\n");
        }

        else if (command == CMD_EXIT) {

            printf("[BACKUP] Arret du serveur secondaire.\n");

            break;
        }
    }

    close(socket_fd);

    exit(EXIT_SUCCESS);
}


// =====================================================
// COMMUNICATION AVEC LE SERVEUR SECONDAIRE
// =====================================================

int backup_socket;


// Envoyer les données.
int send_backup() {

    ServerSnapshot snapshot;

    server_snapshot(&snapshot);

    int command = CMD_BACKUP;

    if (write_full(backup_socket, &command, sizeof(command)) == -1)
        return -1;

    if (write_full(backup_socket, &snapshot, sizeof(snapshot)) == -1)
        return -1;

    int ack;

    if (read_full(backup_socket, &ack, sizeof(ack)) == -1)
        return -1;

    if (ack == 1) {

        printf("[MAIN] Confirmation recue du serveur secondaire.\n");

        return 0;
    }

    return -1;
}


// Recevoir les données.
int receive_backup() {

    int command = CMD_RESTORE;

    ServerSnapshot snapshot;

    if (write_full(backup_socket, &command, sizeof(command)) == -1)
        return -1;

    if (read_full(backup_socket, &snapshot, sizeof(snapshot)) == -1)
        return -1;

    server_restore(&snapshot);

    return 0;
}


// =====================================================
// GESTION DES THREADS
// =====================================================

pthread_t cameras[N_CAMERAS];
pthread_t screens[N_SCREENS];

int camera_ids[N_CAMERAS];
int screen_ids[N_SCREENS];


void start_threads() {

    printf("\n========== DEMARRAGE DES THREADS ==========\n");

    for (int i = 0; i < N_CAMERAS; i++) {

        camera_ids[i] = i + 1;

        pthread_create(
            &cameras[i],
            NULL,
            camera_thread,
            &camera_ids[i]
        );
    }

    for (int i = 0; i < N_SCREENS; i++) {

        screen_ids[i] = i + 1;

        pthread_create(
            &screens[i],
            NULL,
            screen_thread,
            &screen_ids[i]
        );
    }
}


void join_threads() {

    for (int i = 0; i < N_CAMERAS; i++) {
        pthread_join(cameras[i], NULL);
    }

    for (int i = 0; i < N_SCREENS; i++) {
        pthread_join(screens[i], NULL);
    }

    printf("[MAIN] Tous les threads sont arretes.\n");
}


// =====================================================
// MAIN
// =====================================================

int main(int argc, char *argv[]) {

    (void)argc;
    (void)argv;

    srand(time(NULL));

    // -------------------------------------------------
    // Installation du signal.
    // -------------------------------------------------

    struct sigaction sa;

    sa.sa_handler = handle_breakdown;

    sigemptyset(&sa.sa_mask);

    sa.sa_flags = 0;

    sigaction(SIGALRM, &sa, NULL);

    // Panne après un délai aléatoire entre 5 et 15 secondes.
    alarm(rand() % 11 + 5);

    // -------------------------------------------------
    // Initialisation du serveur.
    // -------------------------------------------------

    server_init();

    // -------------------------------------------------
    // Création du serveur secondaire.
    // -------------------------------------------------

    int sockets[2];

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == -1) {

        perror("socketpair");

        return EXIT_FAILURE;
    }

    pid_t pid = fork();

    if (pid < 0) {

        perror("fork");

        return EXIT_FAILURE;
    }

    if (pid == 0) {

        close(sockets[0]);

        backup_server(sockets[1]);
    }

    close(sockets[1]);

    backup_socket = sockets[0];

    printf("[MAIN] Serveur principal demarre.\n");

    printf("[MAIN] Serveur secondaire PID = %d\n", pid);

    // -------------------------------------------------
    // FONCTIONNEMENT NORMAL.
    // -------------------------------------------------

    start_threads();

    printf("\n[MAIN] Systeme de videosurveillance actif.\n");

    // Attendre la panne.
    while (!breakdown_flag) {
        usleep(100000);
    }

    // -------------------------------------------------
    // PANNE DU SERVEUR PRINCIPAL.
    // -------------------------------------------------

    printf("\n\n");
    printf("============================================\n");
    printf("       PANNE DU SERVEUR PRINCIPAL\n");
    printf("============================================\n");

    // Arrêter les threads.
    server_stop();

    join_threads();

    // Désactiver l'alarme.
    alarm(0);

    // -------------------------------------------------
    // SAUVEGARDE.
    // -------------------------------------------------

    printf("\n[MAIN] Envoi des donnees au serveur secondaire.\n");

    if (send_backup() == -1) {

        fprintf(stderr, "[MAIN] Erreur pendant la sauvegarde.\n");

        return EXIT_FAILURE;
    }

    printf("\n[MAIN] Sauvegarde confirmee !\n");

    // -------------------------------------------------
    // RESET.
    // -------------------------------------------------

    printf("\n============================================\n");
    printf("       RESET DU SERVEUR PRINCIPAL\n");
    printf("============================================\n");

    printf("[MAIN] Appuyez sur ENTREE pour redemarrer.\n");

    getchar();

    printf("[MAIN] Redemarrage en cours...\n");

    // Récupération des données.
    if (receive_backup() == -1) {

        fprintf(stderr, "[MAIN] Erreur de restauration.\n");

        return EXIT_FAILURE;
    }

    // Réinitialiser le drapeau de panne.
    breakdown_flag = 0;

    // Annuler toute ancienne alarme.
    alarm(0);

    printf("[MAIN] Serveur principal restaure.\n");

    // -------------------------------------------------
    // REPRISE DU FONCTIONNEMENT.
    // -------------------------------------------------

    printf("\n[MAIN] Reprise du systeme.\n");

    start_threads();

    // Fonctionnement normal après restauration.
    sleep(10);

    // Arrêt propre pour terminer la démonstration.
    server_stop();

    join_threads();

    // -------------------------------------------------
    // ARRÊT DU SERVEUR SECONDAIRE.
    // -------------------------------------------------

    int command = CMD_EXIT;

    write_full(backup_socket, &command, sizeof(command));

    close(backup_socket);

    waitpid(pid, NULL, 0);

    printf("\n============================================\n");
    printf("       FIN DE LA SIMULATION\n");
    printf("============================================\n");

    return EXIT_SUCCESS;
}

