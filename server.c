#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/dir.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <netinet/in.h>

#define HOST_PORT 8080
#define BACKLOG 10
#define BUFFER_SIZE 1024
#define PATH_BUFFER_SIZE 512
#define METHOD_BUFFER 6 /* this probably shouldn't be changed */

int sfd;
char running;

void sendhttpfile(int cfd, const char* file, struct stat* statr)
{
    int fd, readlen;
    double numlen = 1 + log10((double) statr->st_size);
    long alloc = 70 + numlen + 4;
    char fsendbuf[BUFFER_SIZE];

    /* open file */
    if ((fd = open(file, O_RDONLY)) == -1) {
        send(cfd, "HTTP/1.0 500 Internal Server Error\r\nConnection: closed\r\nServer: httpfs\r\n\r\n", 74, 0);
        return;   
    }

    /* allocate space and send file */
    char* httpbuf = (char*) malloc(alloc);

    /* send http response */
    snprintf(httpbuf, alloc, "HTTP/1.0 200 OK\r\nConnection: closed\r\nServer: httpfs\r\nContent-Length: %i\r\n\r\n", statr->st_size);
    
    send(cfd, httpbuf, alloc, 0);
    free(httpbuf);

    /* send file contents */
    while ((readlen = read(fd, fsendbuf, BUFFER_SIZE)) > 0) {
        send(cfd, fsendbuf, readlen, 0);
    }
}

void sendnf(int cfd)
{
    send(cfd, "HTTP/1.0 404 Not Found\r\nConnection: closed\r\nServer: httpfs\r\n\r\nNot Found", 71, 0);
}

void senddirentry(int cfd, const char* entry)
{
    size_t entrylen = strlen(entry);
    size_t buflen = 9 + entrylen + 2 + entrylen + 9;
    char* buf = (char*) malloc(buflen);

    /* send entry as link */
    snprintf(buf, buflen, "<a href=\"%s\">%s</a><br>", entry, entry);
    send(cfd, buf, buflen-1, 0); /* don't send the \0 */
    free(buf);
}

void sendlistdir(int cfd, const char* dirpath)
{
    DIR* dir;
    struct dirent* entry;

    /* open directory */
    if ((dir = opendir(dirpath)) == NULL) {
        /* not found */
        sendnf(cfd);
        return;
    }

    send(cfd, "HTTP/1.0 200 OK\r\nConnection: closed\r\nServer: httpfs\r\nContent-Type: text/html\r\n\r\n", 80, 0);

    /* iterate through directory */
    while ((entry = readdir(dir)) != NULL) {
        senddirentry(cfd, entry->d_name);
    }

    closedir(dir);
}

void handleget(int cfd, const char* path)
{
    /* result of stat */
    struct stat statr;

    if (stat(path, &statr) == 0) {
        /* exists */
        if (S_ISREG(statr.st_mode) || S_ISLNK(statr.st_mode))
            /* send file over http */
            sendhttpfile(cfd, path, &statr);
        else if (S_ISDIR(statr.st_mode))
            /* list directory over http */
            sendlistdir(cfd, path);
    } else {
        /* file does not exit */
        sendnf(cfd);
    }
}

void handleput(int cfd, const char* path)
{

}

void handledel(int cfd, const char* path)
{

}

void handleconn(int cfd, struct sockaddr caddr)
{
    char buf[BUFFER_SIZE];
    char method[METHOD_BUFFER], path[PATH_BUFFER_SIZE];
    int i, spacec = 0, charc = 0, linec = 0, endcharc = 0, fd;
    ssize_t size;

    /* reset buffers */
    for (i = 0; PATH_BUFFER_SIZE > i; i++) path[i] = 0;
    for (i = 0; METHOD_BUFFER > i; i++) method[i] = 0;

    /* receive all bytes */
    do
    {
        size = recv(cfd, buf, BUFFER_SIZE, 0);

        /* iterate through each character and process it */
        for (i = 0; size > i; i++) {
            char c = buf[i];

            switch (c)
            {
            /* count number of spaces */
            case ' ':
                spacec++;
                charc = 0;
                endcharc = 0;
                continue;
            
            /* count number of ending characters in a row */
            case '\n':
                linec++;

            case '\r':
                endcharc++;
                charc = 0;
                break;

            /* reset ending characters in a row counter */
            default:
                endcharc = 0;
                break;
            }

            /* parse request-line */
            switch (spacec)
            {
            case 0:
                /* get request method */
                if (charc > METHOD_BUFFER) continue;
                method[charc] = c;
                break;
            
            case 1:
                /* get path */
                if (charc > PATH_BUFFER_SIZE) continue;
                path[charc] = c;
                break;
            }

            charc++;
        }
    } while (size > 0 && 4 > endcharc); /* 4 end characters in a row is the header-body separator of request */

    /* send back response */
    if (strncmp(method, "GET", 3) == 0)         handleget(cfd, path);
    else if (strncmp(method, "PUT", 3) == 0)    handleput(cfd, path);
    else if (strncmp(method, "DELETE", 6) == 0) handledel(cfd, path);

    /* close client */
    close(cfd);
    shutdown(cfd, SHUT_RDWR);
}

void recvsig(int sig)
{
    /* stop main loop */
    running = 0;

    /* close server socket */
    close(sfd);
    shutdown(sfd, SHUT_RDWR);
}

int main(int argc, char* argv[])
{
    int cfd, caddrlen;
    struct sockaddr_in saddr;
    struct sockaddr caddr;

    /* set running state */
    running = 1;

    /* set signal callback */
    signal(SIGINT, recvsig);

    /* create socket */    
    if ((sfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        fprintf(stderr, "httpfs: can't create socket\n");
        exit(-1);
    }

    /* bind to address */
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(HOST_PORT);

    if (bind(sfd, (struct sockaddr*) &saddr, sizeof(saddr)) < 0) {
        fprintf(stderr, "httpfs: can't bind to port %i\n", HOST_PORT);
        exit(-2);
    }

    /* listen */
    if (listen(sfd, BACKLOG) < 0) {
        fprintf(stderr, "httpfs: could not start listening on port %i\n", HOST_PORT);
        exit(-3);
    }

    /* process loop */
    while (running) {
        if ((cfd = accept(sfd, &caddr, &caddrlen)) < 0 && running) { /* only show warning message when not being shut down */
            /* could not accept connection */
            fprintf(stderr, "httpfs: warn: could not accept connection\n");
            continue;
        }

        /* handle incoming connection */
        handleconn(cfd, caddr);
    }

    return 0;
}