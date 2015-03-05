/* tcp_print2file receives files over a tcp socket and writes to a file
   it is intended as a dummy printer to use with cups
  
   Copyright (C) 2014 Kai Kunschke

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <syslog.h>
#include <netdb.h>

#define LISTEN_ADDR "127.0.0.1"
#define LISTEN_PORT "12345"
#define BACKLOG 4   // number of acceptable connections to queue
#define BUFSIZE 512

#define PRINTOUT_PREFIX "/usb/tcp_fileprinter/"
// #define PRINTOUT_PREFIX "/tmp/print"

static int fd, sd, client;     // printfile, server socket, client socket
static uint8_t fd_open, client_connected;

/* close all open file descriptors, including listener and client sockets */
void sig_handler(int signum)
{
    syslog(LOG_NOTICE, "received signal %d, will close all open file descriptors and exit\n", signum);
    sync();

    if (client_connected)
        if (close(client) == -1) syslog(LOG_WARNING, "signal handler: error closing client socket: %s\n", strerror(errno));

    if (fd_open)
        if (close(fd) == -1) syslog(LOG_WARNING, "signal handler: error closing fd: %s\n", strerror(errno));

    if (close(sd) == -1) syslog(LOG_WARNING, "signal handler: error closing socket: %s\n", strerror(errno));

    exit(EXIT_SUCCESS);
}

/* install signal handlers
 * ignore {SIGTSTP, SIGTTIN, SIGTTOU}
 * finish on {SIGHUP, SIGINT, SIGQUIT}
 */
void install_sighandlers()
{
    struct sigaction sigact;
    int signals[] = {SIGHUP, SIGINT, SIGTERM};  // signals to handle by sig_handler()
    int signals_ignore[] = {SIGTSTP, SIGTTIN, SIGTTOU};  // signals to ignore
    sigact.sa_handler = sig_handler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    for (unsigned int i = 0; i < sizeof(signals) / sizeof(int); ++i) {
        if (sigaction(signals[i], &sigact, NULL) == -1) {
            fprintf(stderr, "error installing signal handler: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }

    sigact.sa_handler = SIG_IGN;
    for (unsigned int i = 0; i < sizeof(signals_ignore) / sizeof(int); ++i) {
        if (sigaction(signals_ignore[i], &sigact, NULL) == -1) {
            fprintf(stderr, "error installing signal handler: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
}

/* daemonize process and open syslog */
void daemonize(char *program_name, int facility)
{
    pid_t pid;

    switch (pid = fork()) {
        case -1:
            fprintf(stderr, "error on fork(): %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        case 0:     // child process
            openlog(program_name, LOG_CONS, facility);
            break;
        default:    // parent process
            exit(EXIT_SUCCESS);
    }

    if (setsid() == -1) {
        syslog(LOG_ERR, "error on setsid(): %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    switch(pid = fork()) {
        case -1:
            syslog(LOG_ERR, "error on fork(): %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        case 0:
            break;
        default:
            exit(EXIT_SUCCESS);
    }

    // close unnecessary file descriptors to terminal
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    // change settings inherited from starting terminal
    if (chdir("/") == -1) syslog(LOG_WARNING, "error on chdir(): %s\n", strerror(errno));
    umask(0);
}

int main(int argc, char *argv[])
{
    char msg[BUFSIZE] = "", filename[64] = "";
    int retval, bytes_read = 0, random = 0;
    time_t t = time(NULL);
    struct tm *tm;
    char timestamp[22];
    char *progname = basename(argv[0]);
    struct addrinfo *ai, hints;

    if (argc > 1) printf("%s does not take any arguments\n", progname);

    install_sighandlers();
    daemonize(progname, LOG_DAEMON);

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if ((retval = getaddrinfo(LISTEN_ADDR, LISTEN_PORT, &hints, &ai))) {
        syslog(LOG_ERR, "error on getaddrinfo(): %s\n", gai_strerror(retval));
        exit(EXIT_FAILURE);
    }

    if ((sd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
        syslog(LOG_ERR, "error on socket(): %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if ((retval = bind(sd, ai->ai_addr, ai->ai_addrlen)) == -1) {
        syslog(LOG_ERR, "error on bind(): %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    if ((retval = listen(sd, BACKLOG)) == -1) {
        syslog(LOG_ERR, "error on listen(): %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    syslog(LOG_INFO, "successfully started %s\n", progname);

    while(1)
    {
        if ((client = accept(sd, NULL, NULL)) == -1) {
            syslog(LOG_WARNING, "error on accept(): %s\n", strerror(errno));
            continue;
        }

        syslog(LOG_INFO, "accepted new print client"); // also get current timestamp
        client_connected = 1;

        tm = localtime(&t);
        if ((tm != NULL) && strftime(timestamp, sizeof(timestamp), "%d.%m.%Y-%T", tm)) {
            snprintf(filename, sizeof(filename), "%s%s", PRINTOUT_PREFIX, timestamp);
        } else {
            syslog(LOG_WARNING, "error getting current time\n");
            do {
                random = rand();
                snprintf(filename, sizeof(filename), "%sfile-%i", PRINTOUT_PREFIX, random);
            } while ( access(filename, F_OK) != -1 ); // create unique random filename 
        }

        syslog(LOG_INFO, "start printing to %s", filename);
        if ((fd = open(filename, O_CREAT|O_WRONLY, S_IRUSR|S_IWUSR)) == -1) {
            syslog(LOG_WARNING, "error opening printfile %s: %s", filename, strerror(errno));
            continue;
        }

        fd_open = 1;
        bytes_read = 0;
        while((bytes_read = read(client, msg, BUFSIZE)) > 0) write(fd, msg, bytes_read);

        syslog(LOG_INFO, "done printing to %s", filename);
        memset(msg, 0, BUFSIZE);    // eliminate false evidence in memory
        close(fd);
        close(client);
        fd_open = 0;
        client_connected = 0;
    }

    close(sd);

    return EXIT_SUCCESS;
}
