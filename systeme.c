#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

// -------- configuration --------
#define BUFFER_SIZE 5
#define N_CAMERAS 3
#define N_SCREENS 2
// Rappel de l'énoncé : le total caméras + écrans doit être > 4.

// ------------------- Simulation de caméra/écran (fournies, ne pas modifier) -------------------
static int video_id = 0;
char *createVideo() {
    char *buf = malloc(64);
    if (!buf) { perror("malloc"); exit(1); }
    sprintf(buf, "Video-%d", video_id++);
    return buf;
}
void broadcastVideo(const char *video) {
    printf("[SCREEN] Diffusion: %s\n", video);
}

// ------------------- Signal : simulation de panne (fourni) -------------------
// alarm() est déclenché avec un délai aléatoire dans main() : quand il expire,
// ce handler est appelé. Ne fais PAS de travail lourd dans un handler de
// signal (voir le cours) : contente-toi de lever un drapeau.
volatile sig_atomic_t breakdown_flag = 0;

void handle_breakdown(int sig) {
    (void)sig;
    breakdown_flag = 1;
}

/* TODO (Section 1) : Mise en place du système de vidéosurveillance
 * ------------------------------------------------------------------
 *
 * TODO (Section 2) : Intégrité des données lors d'une panne
 * ------------------------------------------------------------------
 */

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    srand(time(NULL));

    struct sigaction sa;
    sa.sa_handler = handle_breakdown;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGALRM, &sa, NULL);

    // Simulation d'une panne du serveur principal après un délai aléatoire.
    alarm(rand() % 11 + 5);


    return 0;
}
