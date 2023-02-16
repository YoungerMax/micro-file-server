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
#define HEADER_NAME_SIZE 48
#define HEADER_VALUE_SIZE 1024
#define MAX_HEADER_COUNT 64
#define METHOD_BUFFER_SIZE 6 /* this probably shouldn't be changed */
#define STATUS_CODE_SIZE 3 /* this is for readability, also probably shouldn't be changed */
#define DIRLEN(entrylen) (entrylen * 2 + 20)

int sfd;
char running;

enum expecting
{
	E_METHOD,
	E_PATH,
	E_HTTP_VER,
	E_NEW_LINE,
	E_HEADER_NAME,
	E_HEADER_NV_SPACE, /* space after the colon between the name and value*/
	E_HEADER_VAL
};

enum method
{
	M_GET = 1,
	M_PUT,
	M_DELETE
};

struct header_t
{
	char name[HEADER_NAME_SIZE];
	char value[HEADER_VALUE_SIZE];
};

struct request_t
{
	enum method method;

	char path[PATH_BUFFER_SIZE];
	short psize;

	struct header_t headers[MAX_HEADER_COUNT];
	short hsize;
};

void printreq(struct request_t req)
{
	int i;

	printf("method %u\npath %s\nhsize %i\n", req.method, req.path, req.hsize);

	for (i = 0; req.hsize > i; i++)
		printf("%s: %s\n", req.headers[i].name, req.headers[i].value);
}

int getheaderindex(struct request_t req, const char* header)
{
	int i;

	for (i = 0; req.hsize > i; i++)
		if (strcmp(req.headers[i].name, header)) /* TODO: double check the security of this */
			return i;

	return -1;
}

void sendres(int cfd, const char code[STATUS_CODE_SIZE], const char* text, const char* contenttype, long contentlen)
{
	/* length of contentlen as a string*/
	double lenstr = 1 + log10((double) contentlen);

	/* allocate memory for response */
	int sendlen = 9 + STATUS_CODE_SIZE + 1 + strlen(text) + 52 + strlen(contenttype) + 18 + lenstr + 4 + 1 /* +1 for \0 */;
	char* buf = (char*) malloc(sendlen);

	/* format response and send */
	snprintf(buf, sendlen, "HTTP/1.0 %s %s\r\nConnection: closed\r\nServer: httpfs\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n", code, text, contenttype, contentlen);

	send(cfd, buf, sendlen - 1 /* -1 to remove the \0 */, 0);
	free(buf);
}

void sendrescntnt(int cfd, const char code[STATUS_CODE_SIZE], const char* text, const char* contenttype, const char* content)
{
	sendres(cfd, code, text, contenttype, strlen(content));
	send(cfd, content, strlen(content), 0);
}

void sendhttpfile(int cfd, const char* file, struct stat* statr)
{
	int fd, readlen;
	char fsendbuf[BUFFER_SIZE];

	/* open file */
	if ((fd = open(file, O_RDONLY)) == -1) {
		sendrescntnt(cfd, "500", "Internal Server Error", "text/html", "Can't open file");
		return;
	}

	/* send http response */
	sendres(cfd, "200", "OK", "application/octet-stream", statr->st_size);

	/* send file contents */
	while ((readlen = read(fd, fsendbuf, BUFFER_SIZE)) > 0)
		send(cfd, fsendbuf, readlen, 0);

	close(fd);
}

void sendnf(int cfd)
{
	sendrescntnt(cfd, "404", "Not Found", "text/html", "Not Found");
}

void senddirentry(int cfd, const char* entry)
{
	size_t entrylen = strlen(entry);
	size_t buflen = DIRLEN(entrylen);
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
	int size = 0;

	/* open directory */
	if ((dir = opendir(dirpath)) == NULL) {
		/* not found */
		sendnf(cfd);

		return;
	}

	/* count for content length */
	while ((entry = readdir(dir)) != NULL)
		size += DIRLEN(strlen(entry->d_name));

	/* start http response */
	sendres(cfd, "200", "OK", "text/html", size);
	rewinddir(dir);

	/* iterate through directory and send as HTML */
	while ((entry = readdir(dir)) != NULL)
		senddirentry(cfd, entry->d_name);
		
	/* clean up directory */
	closedir(dir);
}

void handleget(int cfd, struct request_t req)
{
	/* result of stat */
	struct stat statr;

	if (stat(req.path, &statr) == 0) {
		/* exists */
		if (S_ISREG(statr.st_mode) || S_ISLNK(statr.st_mode)) {
			/* send file over http */
			sendhttpfile(cfd, req.path, &statr);
		} else if (S_ISDIR(statr.st_mode)) {
			/* list directory over http */
			sendlistdir(cfd, req.path);
		}
	} else {
		/* file does not exit */
		sendnf(cfd);
	}
}

void handleput(int cfd, struct request_t req)
{
	int readlen, fd, clenheaderi;
	long read, contentlen;
	char buf[BUFFER_SIZE];

	/* open file for writing, create it */
	if (creat(req.path, 0666) < 0) {
		sendrescntnt(cfd, "500", "Internal Server Error", "text/html", "Can't create file");
	}

	if ((fd = open(req.path, O_WRONLY)) == -1) {
		/* could not open */
		sendrescntnt(cfd, "500", "Internal Server Error", "text/html", "Can't open file");

		return;
	}

	/* get content length */
	if ((clenheaderi = getheaderindex(req, "Content-Length")) < 0) {
		close(fd);
		sendrescntnt(cfd, "400", "Bad Request", "text/html", "Expected Content-Length header");
		return;
	}
	
	contentlen = atol(req.headers[clenheaderi].value);
	
	/* write to filesystem */
	while ((readlen = recv(cfd, buf, BUFFER_SIZE, 0)) > 0 && contentlen > read) {
		read += readlen;
		write(fd, buf, readlen);
	}

	close(fd);
}

void handledel(int cfd, struct request_t req)
{

}

struct request_t parseconn(int cfd, struct sockaddr caddr)
{
	ssize_t size;
	char buf[BUFFER_SIZE];
	char method[METHOD_BUFFER_SIZE], path[PATH_BUFFER_SIZE], hname[HEADER_NAME_SIZE], hvalue[HEADER_VALUE_SIZE];
	int i, charc = 0, endcharc = 0, linec = 0, hnamec = 0, hvaluec = 0, fd;
	enum expecting current = E_METHOD;
	struct request_t req = {};

	/* reset buffers */
	/* TOOD: replace with memset? */
	memset(path, 0, sizeof(path));
	memset(method, 0, sizeof(method));

	/* receive all bytes */
	do
	{
		size = recv(cfd, buf, BUFFER_SIZE, 0);

		/* iterate through each character and process it */
		for (i = 0; size > i; i++) {
			char c = buf[i];
			
			switch (current) {
				case E_METHOD:
					if (c == ' ') {
						current = E_PATH;
						charc = 0;
						continue;
					} else if (METHOD_BUFFER_SIZE > charc) {
						method[charc] = c;
					} else {
						/* TODO: too big, handle! */
					}

					break;

				case E_PATH:
					if (c == ' ') {
						current = E_HTTP_VER;
						charc = 0;
						continue;
					} else if (PATH_BUFFER_SIZE > charc) {
						path[charc] = c;
						req.psize++;
					} else {
						/* TODO: too big, handle */
					}

					break;

				case E_HTTP_VER:
					if (c == '\r') {
						current = E_NEW_LINE;
						endcharc++;
					}

					/* skip processing */

					break;

				case E_NEW_LINE:
					if (c == '\n') {
						current = E_HEADER_NAME;
						endcharc++;
						linec++;

						memset(hname, 0, sizeof(hname));
						memset(hvalue, 0, sizeof(hvalue));
					}

					break;

				case E_HEADER_NAME:
					if (c == '\r') {
						current = E_NEW_LINE;
						endcharc++;
					} else {
						endcharc = 0;

						if (c == ':') {
							current = E_HEADER_NV_SPACE;
						} else if (MAX_HEADER_COUNT > linec - 1 && HEADER_NAME_SIZE > hnamec) {
							/* add to header buffer */
							hname[hnamec++] = c;
						} else {
							/* TODO: too big, handle */
						}
					}

					break;

				case E_HEADER_NV_SPACE:
					if (c == ' ') {
						current = E_HEADER_VAL;
					}

					break;

				case E_HEADER_VAL:
					if (c == '\r') {
						current = E_NEW_LINE;
						endcharc++;

						/* add header */
						if (MAX_HEADER_COUNT > linec - 1) {
							strncpy(req.headers[linec - 1].name, hname, hnamec);
							strncpy(req.headers[linec - 1].value, hvalue, hvaluec);
							hnamec = 0;
							hvaluec = 0;
	
							req.hsize++;
						}
					} else if (MAX_HEADER_COUNT > linec - 1 && HEADER_VALUE_SIZE > hvaluec) {
						endcharc = 0;

						/* add to header value buffer */
						hvalue[hvaluec++] = c;
					} else {
						/* TODO: too big, handle */
					}

					break;

				default:
					break;
			}
			
			charc++;
		}
	} while (size > 0 && 4 > endcharc); /* 4 end characters in a row is the header-body separator of request */

	/* set method */
	if (strncmp(method, "GET", 3) == 0) req.method = M_GET;
	else if (strncmp(method, "PUT", 3) == 0) req.method = M_PUT;
	else if (strncmp(method, "DELETE", 6) == 0) req.method = M_DELETE;

	/* set path */
	strncpy(req.path, path, req.psize);

	return req;
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
	int cfd;
	socklen_t caddrlen;
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
		/* parse it */
		struct request_t req = parseconn(cfd, caddr);

		printreq(req);

		/* route it & send back response */
		switch (req.method) {
			case M_GET:
				handleget(cfd, req);
				break;

			case M_PUT:
				handleput(cfd, req);
				break;

			case M_DELETE:
				handledel(cfd, req);
				break;

			default:
				sendrescntnt(cfd, "405", "Method Not Allowed", "text/html", "Method not allowed; only GET, PUT, and DELETE are allowed.");
				break;

		}

		/* close client */
		close(cfd);
		shutdown(cfd, SHUT_RDWR);
	}

	return 0;
}
