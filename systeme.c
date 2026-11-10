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

// CONFIGURATION
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

// FONCTIONS FOURNIES Par l'ennonce
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

// SIGNAL DE PANNE
volatile sig_atomic_t breakdown_flag = 0;

void handle_breakdown(int sig) {
    (void)sig;
    breakdown_flag = 1;
}

// COMMUNICATION ENTRE PROCESSUS
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

// MONITEUR DE MESA
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} MesaMonitor;

// initialiser le moniteur
void monitor_init(MesaMonitor *m) {
    pthread_mutex_init(&m->mutex, NULL);
    pthread_cond_init(&m->not_empty, NULL);
    pthread_cond_init(&m->not_full, NULL);
}

// entrer dans le moniteur
void monitor_enter(MesaMonitor *m) {
    pthread_mutex_lock(&m->mutex);
}

// sortir du moniteur
void monitor_leave(MesaMonitor *m) {
    pthread_mutex_unlock(&m->mutex);
}

// SERVEUR PRINCIPAL
typedef struct {
    char video[VIDEO_SIZE];
    int state;
} VideoSlot;

typedef struct {
    VideoSlot slots[BUFFER_SIZE];
    int stopping;
    MesaMonitor monitor;
} Server;

Server server;

// initialisation
void server_init() {
    server.stopping = 0;
    for (int i = 0; i < BUFFER_SIZE; i++) {
        server.slots[i].state = FREE;
        server.slots[i].video[0] = '\0';
    }
    monitor_init(&server.monitor);
}

// Cherche une place qui soit libre
int find_free_slot() {
    for (int i = 0; i < BUFFER_SIZE; i++) {
        if (server.slots[i].state == FREE)
            return i;
    }
    return -1;
}

//chercher une video dipsonible
int find_ready_slot() {
    for (int i = 0; i < BUFFER_SIZE; i++) {
        if (server.slots[i].state == READY)
            return i;
    }
    return -1;
}

// PRODUCTEUR: AJOUTER UNE VIDEO
int server_add_video(const char *video) {
    MesaMonitor *m = &server.monitor;
    monitor_enter(m);
    while (find_free_slot() == -1 && !server.stopping) {
        pthread_cond_wait(&m->not_full, &m->mutex);
    }
    if (server.stopping) {
        monitor_leave(m);
        return -1;
    }
    int index = find_free_slot();
    strcpy(server.slots[index].video, video);
    server.slots[index].state = READY;
    printf("[SERVER] Video ajoutee : %s\n", video);
    pthread_cond_signal(&m->not_empty);
    monitor_leave(m);
    return 0;
}

// CONSOMATEUR: RESERVER UNE VIDEO
int server_get_video(char *video) {
    MesaMonitor *m = &server.monitor;
    monitor_enter(m);
    while (find_ready_slot() == -1 && !server.stopping) {
        pthread_cond_wait(&m->not_empty, &m->mutex);
    }
    if (server.stopping) {
        monitor_leave(m);
        return -1;
    }
    int index = find_ready_slot();
    strcpy(video, server.slots[index].video);
    
    // La video n'est pas supprimee juste reservee
    server.slots[index].state = IN_USE;
    printf("[SERVER] Reservation : %s\n", video);
    monitor_leave(m);
    return index;
}

// SECTION 2: VALIDATION DE LA DIFFUSION
void server_commit_video(int index) {
    MesaMonitor *m = &server.monitor;
    monitor_enter(m);
    if (!breakdown_flag && !server.stopping) {
        server.slots[index].state = FREE;
        server.slots[index].video[0] = '\0';
        printf("[SERVER] Video validee et supprimee\n");
        pthread_cond_signal(&m->not_full);
    }
    monitor_leave(m);
}

//ARRET DU SERVEUR
void server_stop() {
    MesaMonitor *m = &server.monitor;
    monitor_enter(m);
    server.stopping = 1;
    pthread_cond_broadcast(&m->not_full);
    pthread_cond_broadcast(&m->not_empty);
    monitor_leave(m);
}

// CAMERAS
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

// ECRANS
void *screen_thread(void *arg) {
    int id = *(int *)arg;
    while (1) {
        char video[VIDEO_SIZE];
        int index = server_get_video(video);
        if (index == -1)
            break;

        // VERIFIER LA PANNE AVANT DE COMMENCER LA DIFFUSION
        if (breakdown_flag) {
            printf("[SCREEN %d] Panne detectee avant diffusion de %s. Video conservee pour reprise.\n", id, video);
            break; 
        }

        printf("[SCREEN %d] Reception : %s\n", id, video);

        // SIMULATION DE LA DIFFUSION
        usleep(1500000);

        // VERIFIER LA PANNE APRES DIFFUSION
        if (breakdown_flag) {
            printf("[SCREEN %d] Panne survenue pendant la diffusion de %s.\n", id, video);
            server_commit_video(index);
            break;
        }

        // Diffusion reussie sans panne
        broadcastVideo(video);
        server_commit_video(index);
    }

    printf("[SCREEN %d] Arret.\n", id);
    return NULL;
}

// SAUVEGARDE ET RESTAURATION
typedef struct {
    VideoSlot slots[BUFFER_SIZE];
} ServerSnapshot;

// Faire une copie coherente du serveur
//On fait le choix que si une video est interromue pendant sa lecture elle est consideree comme lue
void server_snapshot(ServerSnapshot *snapshot) {
    MesaMonitor *m = &server.monitor;
    monitor_enter(m);
    for (int i = 0; i < BUFFER_SIZE; i++) {
        snapshot->slots[i] = server.slots[i];
        if (snapshot->slots[i].state == IN_USE) {
            snapshot->slots[i].state = READY;
        }
    }
    monitor_leave(m);
}

// Restaurer le serveur principal
void server_restore(ServerSnapshot *snapshot) {
    MesaMonitor *m = &server.monitor;
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

// SERVEUR SECONDAIRE
void backup_server(int socket_fd) {
    ServerSnapshot backup;
    printf("[BACKUP] Serveur secondaire demarre.\n");
    while (1) {
        int command;
        if (read_full(socket_fd, &command, sizeof(command)) == -1)
            break;

        if (command == CMD_BACKUP) {
            printf("[BACKUP] Reception des donnees...\n");
            if (read_full(socket_fd, &backup, sizeof(backup)) == -1)
                break;
            printf("[BACKUP] Copie terminee\n");
            
            int ack = 1;
            if (write_full(socket_fd, &ack, sizeof(ack)) == -1)
                break;
            printf("[BACKUP] Confirmation ACK envoyee.\n");
        }
        else if (command == CMD_RESTORE) {
            printf("[BACKUP] Envoi des donnees au serveur principal.\n");
            if (write_full(socket_fd, &backup, sizeof(backup)) == -1)
                break;
            printf("[BACKUP] backup retransmise\n");
        }
        else if (command == CMD_EXIT) {
            printf("[BACKUP] Arret du serveur secondaire.\n");
            break;
        }
    }
    close(socket_fd);
    exit(EXIT_SUCCESS);
}

// COMUNICATION AVEC LE SERVEUR SECONDAIRE
int backup_socket;

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

// GESTION DES THREADS
pthread_t cameras[N_CAMERAS];
pthread_t screens[N_SCREENS];

int camera_ids[N_CAMERAS];
int screen_ids[N_SCREENS];

void start_threads() {
    printf("\n========== DEMARRAGE DES THREADS ==========\n");
    for (int i = 0; i < N_CAMERAS; i++) {
        camera_ids[i] = i + 1;
        pthread_create(&cameras[i], NULL, camera_thread, &camera_ids[i]);
    }
    for (int i = 0; i < N_SCREENS; i++) {
        screen_ids[i] = i + 1;
        pthread_create(&screens[i], NULL, screen_thread, &screen_ids[i]);
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

// MAIN
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    srand(time(NULL));
    
    struct sigaction sa;
    sa.sa_handler = handle_breakdown;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);

    // Panne après un délai aléatoire entre 5 et 15 secondes.
    alarm(rand() % 11 + 5);

    // Initialisation des serveurs
    server_init();
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

    // FONCTIONNEMENT NORMAL
    start_threads();
    printf("\n[MAIN] Systeme de video surveillance actif.\n");
    while (!breakdown_flag) {
        usleep(100000);
    }

    // PANNE DU SERVEUR PRINCIPAL
    printf("\n\n");
    printf("============================================\n");
    printf("       PANNE DU SERVEUR PRINCIPAL\n");
    printf("============================================\n");

    // Arreter les threads et faire une backup
    server_stop();
    join_threads();
    alarm(0);
    printf("\n[MAIN] Envoi des donnees au serveur secondaire.\n");
    if (send_backup() == -1) {
        fprintf(stderr, "[MAIN] Erreur pendant la sauvegarde.\n");
        return EXIT_FAILURE;
    }
    printf("\n[MAIN] Sauvegarde confirmee !\n");

    // RESET
    printf("\n============================================\n");
    printf("       RESET DU SERVEUR PRINCIPAL\n");
    printf("============================================\n");
    printf("[MAIN] Appuyez sur ENTREE pour redemarrer.\n");
    getchar();
    printf("[MAIN] Redemarrage en cours...\n");

    if (receive_backup() == -1) {
        fprintf(stderr, "[MAIN] Erreur de restauration.\n");
        return EXIT_FAILURE;
    }
    breakdown_flag = 0;
    alarm(0);
    printf("[MAIN] Serveur principal restaure.\n");

    // REPRISE DU FONCTIONNEMENT NORMALEMENT
    printf("\n[MAIN] Reprise du systeme.\n");
    start_threads();
    sleep(10);
    
    // Arret propre des serveurs pour terminer
    server_stop();
    join_threads();

    int command = CMD_EXIT;
    write_full(backup_socket, &command, sizeof(command));
    close(backup_socket);
    waitpid(pid, NULL, 0);
    printf("\n============================================\n");
    printf("       FIN DE LA SIMULATION\n");
    printf("============================================\n");
    return EXIT_SUCCESS;
}
