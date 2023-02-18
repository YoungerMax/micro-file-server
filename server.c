#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stddef.h>
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

/* this probably shouldn't be changed */
#define METHOD_BUFFER_SIZE 6

/* these should not be changed; they are for readability */
#define STATUS_CODE_SIZE 3
#define HTTP_VERSION_SIZE 8
#define HEADER_BUFFER_SIZE HEADER_NAME_SIZE + 2 + HEADER_VALUE_SIZE + 2
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
	M_GET,
	M_PUT,
	M_DELETE
};

enum http_version
{
	V_09,
	V_10,
	V_11
};

struct header_t
{
	char name[HEADER_NAME_SIZE];
	char value[HEADER_VALUE_SIZE];
};

struct request_t
{
	enum method method;
	enum http_version http_version;

	char path[PATH_BUFFER_SIZE];
	short psize;

	struct header_t headers[MAX_HEADER_COUNT];
	short hsize;
};

struct response_t
{
	enum http_version http_version;
	
	const char* status_code;
	const char* status_text;
	
	struct header_t headers[MAX_HEADER_COUNT];
	short header_count;
};

/* common headers */
const struct header_t H_CONNECTION_CLOSED = {
	.name = "Connection",
	.value = "closed"
};

const struct header_t H_CONNECTION_KEEPALIVE = {
	.name = "Connection",
	.value = "keep-alive"
};

const struct header_t H_SERVER = {
	.name = "Server",
	.value = "httpfs"
};


int get_header_index(struct request_t req, const char* header)
{
	int i;

	for (i = 0; req.hsize > i; i++)
		if (strcmp(req.headers[i].name, header) == 0) /* TODO: double check the security of this */
			return i;

	return -1;
}

char* http_version_as_string(enum http_version http_version)
{
	switch (http_version) {
		case V_09:
			return "HTTP/0.9";
		
		case V_10:
			return "HTTP/1.0";
		
		case V_11:
			return "HTTP/1.1";
	}
}

char* method_as_string(enum method method)
{
	switch (method) {
		case M_GET:
			return "GET";

		case M_PUT:
			return "PUT";

		case M_DELETE:
			return "DELETE";
	}
}

void print_request(struct request_t req)
{
	int i;

	printf("%s %s %s\r\n", method_as_string(req.method), req.path, http_version_as_string(req.http_version));

	for (i = 0; req.hsize > i; i++)
		printf("%s: %s\r\n", req.headers[i].name, req.headers[i].value);
	
	printf("\r\n");
}

size_t get_response_length(struct response_t res)
{
	int i;
	size_t s = 0;

	s += HTTP_VERSION_SIZE + 1 + STATUS_CODE_SIZE + 1 + strlen(res.status_text) + 2;
	
	for (i = 0; res.header_count > i; i++)
		s += strlen(res.headers[i].name) + 2 + strlen(res.headers[i].value) + 2;
	
	s += 2;
	
	return s;
}

void send_response(int cfd, struct response_t response)
{
	int i;
	size_t response_length = 0, header_size;
	char* response_buffer = (char*) malloc(get_response_length(response));

	response_length += snprintf(response_buffer + response_length, HTTP_VERSION_SIZE + 1 + STATUS_CODE_SIZE + 1 + strlen(response.status_text) + 2 + 1, "%s %s %s\r\n", http_version_as_string(response.http_version), response.status_code, response.status_text);

	for (i = 0; response.header_count > i; i++) {
		header_size = strlen(response.headers[i].name) + 2 + strlen(response.headers[i].value) + 2 + 1;
		response_length += snprintf(response_buffer + response_length, header_size, "%s: %s\r\n", response.headers[i].name, response.headers[i].value);
	}

	response_length += snprintf(response_buffer + response_length, 3, "\r\n");

	send(cfd, response_buffer, response_length, 0);
	free(response_buffer);
}

void send_response_with_content_length(int cfd, const char status_code[STATUS_CODE_SIZE], const char* status_text, const char* content_type, long content_length)
{
	int i;

	/* length of contentlen as a string */
	long length_of_content_length = (long) 1 + log10((double) content_length) + 1;
	char* content_length_buffer = (char*) malloc(length_of_content_length);
	snprintf(content_length_buffer, length_of_content_length, "%ld", content_length);

	/* allocate memory for response */
	struct response_t response = {
		.http_version = V_11,
		.status_code = status_code,
		.status_text = status_text
	};

	/* content-type header */
	struct header_t h_content_type = {
		.name = "Content-Type"
	};
	
	strncpy(h_content_type.value, content_type, strnlen(content_type, HEADER_NAME_SIZE));
	
	/* content-length header */
	struct header_t h_content_length = {
		.name = "Content-Length"
	};
	
	strncpy(h_content_length.value, content_length_buffer, strnlen(content_length_buffer, HEADER_VALUE_SIZE));

	/* construct response */
	response.headers[0] = H_CONNECTION_CLOSED;
	response.headers[1] = H_SERVER;
	response.headers[2] = h_content_type;
	response.headers[3] = h_content_length;
	response.header_count = 4;

	/* send response and clean up */
	send_response(cfd, response);
	free(content_length_buffer);
}

void send_response_with_content(int cfd, const char status_code[STATUS_CODE_SIZE], const char* status_text, const char* content_type, const char* content)
{
	send_response_with_content_length(cfd, status_code, status_text, content_type, strlen(content));
	send(cfd, content, strlen(content), 0);
}

void send_http_file(int cfd, const char* file_path, size_t file_size)
{
	int fd, read_length;
	char file_send_buffer[BUFFER_SIZE];

	/* open file */
	if ((fd = open(file_path, O_RDONLY)) == -1) {
		send_response_with_content(cfd, "500", "Internal Server Error", "text/html", "Can't open file");
		return;
	}

	/* send http response */
	send_response_with_content_length(cfd, "200", "OK", "application/octet-stream", file_size);

	/* send file contents */
	while ((read_length = read(fd, file_send_buffer, BUFFER_SIZE)) > 0)
		send(cfd, file_send_buffer, read_length, 0);

	close(fd);
}

void send_not_found(int cfd)
{
	send_response_with_content(cfd, "404", "Not Found", "text/html", "Not found");
}

void send_directory_entry(int cfd, const char* entry)
{
	size_t entry_length = strlen(entry);
	size_t buffer_length = DIRLEN(entry_length);
	char* send_buffer = (char*) malloc(buffer_length);

	/* send entry as link */
	snprintf(send_buffer, buffer_length, "<a href=\"%s\">%s</a><br>", entry, entry);
	send(cfd, send_buffer, buffer_length - 1, 0); /* don't send the \0 */
	free(send_buffer);
}

void send_directory_listing(int cfd, const char* directory_path)
{
	DIR* dir;
	struct dirent* entry;
	size_t size = 0;

	/* open directory */
	if ((dir = opendir(directory_path)) == NULL) {
		/* not found */
		send_not_found(cfd);

		return;
	}

	/* count for content length */
	while ((entry = readdir(dir)) != NULL)
		size += DIRLEN(strlen(entry->d_name)) - 1;
	
	/* start http response */
	send_response_with_content_length(cfd, "200", "OK", "text/html", size);
	rewinddir(dir);

	/* iterate through directory and send as HTML */
	while ((entry = readdir(dir)) != NULL)
		send_directory_entry(cfd, entry->d_name);
		
	/* clean up directory */
	closedir(dir);
}

void handle_get_request(int cfd, struct request_t req)
{
	/* result of stat */
	struct stat stat_result;

	if (stat(req.path, &stat_result) == 0) {
		/* exists */
		if (S_ISREG(stat_result.st_mode) || S_ISLNK(stat_result.st_mode)) {
			/* send file over http */
			send_http_file(cfd, req.path, stat_result.st_size);
		} else if (S_ISDIR(stat_result.st_mode)) {
			/* list directory over http */
			send_directory_listing(cfd, req.path);
		}
	} else {
		/* file does not exit */
		send_not_found(cfd);
	}
}

void handle_put_request(int cfd, struct request_t req)
{
	int read_length, fd, header_index;
	long read_bytes, content_length;
	char buffer[BUFFER_SIZE];

	/* open file for writing, create it */
	if (creat(req.path, 0666) < 0) {
		send_response_with_content(cfd, "500", "Internal Server Error", "text/html", "Can't create file");
	}

	if ((fd = open(req.path, O_WRONLY)) == -1) {
		/* could not open */
		send_response_with_content(cfd, "500", "Internal Server Error", "text/html", "Can't open file");

		return;
	}

	/* get content length */
	if ((header_index = get_header_index(req, "Content-Length")) == -1) {		
		close(fd);
		send_response_with_content(cfd, "400", "Bad Request", "text/html", "Expected Content-Length header");
		return;
	}
	
	content_length = atol(req.headers[header_index].value);

	/* get expect header */
	if ((header_index = get_header_index(req, "Expect")) != -1) {
		/* only directive is `100-continue` */
		struct response_t continue_response = {
			.http_version = V_11,
			.status_code = "100",
			.status_text = "Continue"
		};

		continue_response.headers[0] = H_CONNECTION_CLOSED;
		continue_response.headers[1] = H_SERVER;
		continue_response.header_count = 2;
		
		send_response(cfd, continue_response);
	}

	/* write to filesystem */
	while ((read_length = recv(cfd, buffer, BUFFER_SIZE, 0)) > 0) {
		read_bytes += read_length;
		write(fd, buffer, read_length);

		if (read_bytes >= content_length) break;
	}

	send_response_with_content(cfd, "201", "Created", "text/html", "Created");	

	close(fd);
}

void handle_delete_request(int cfd, struct request_t req)
{

}

struct request_t parse_request(int cfd, struct sockaddr client_address)
{
	ssize_t size;
	char buffer[BUFFER_SIZE];
	char method[METHOD_BUFFER_SIZE], path[PATH_BUFFER_SIZE], http_version[HTTP_VERSION_SIZE], header_name[HEADER_NAME_SIZE], header_value[HEADER_VALUE_SIZE];
	int i, char_count = 0, end_char_count = 0, line_count = 0, header_name_count = 0, header_value_count = 0, fd;
	enum expecting current = E_METHOD;
	struct request_t req = {};

	/* reset buffers */
	memset(path, 0, sizeof(path));
	memset(method, 0, sizeof(method));
	memset(http_version, 0, sizeof(http_version));

	/* receive all bytes */
	do
	{
		size = recv(cfd, buffer, BUFFER_SIZE, 0);

		/* iterate through each character and process it */
		for (i = 0; size > i; i++) {
			char c = buffer[i];
			
			switch (current) {
				case E_METHOD:
					if (c == ' ') {
						current = E_PATH;
						char_count = 0;
						continue;
					} else if (METHOD_BUFFER_SIZE > char_count) {
						method[char_count] = c;
					} else {
						/* TODO: too big, handle! */
					}

					break;

				case E_PATH:
					if (c == ' ') {
						current = E_HTTP_VER;
						char_count = 0;
						continue;
					} else if (PATH_BUFFER_SIZE > char_count) {
						path[char_count] = c;
						req.psize++;
					} else {
						/* TODO: too big, handle */
					}

					break;

				case E_HTTP_VER:
					if (c == '\r') {
						current = E_NEW_LINE;
						end_char_count++;
					} else if (HTTP_VERSION_SIZE > char_count) {
						http_version[char_count] = c;
					} else {
						/* TODO: handle too big */
					}

					break;

				case E_NEW_LINE:
					if (c == '\n') {
						current = E_HEADER_NAME;
						end_char_count++;
						line_count++;

						memset(header_name, 0, sizeof(header_name));
						memset(header_value, 0, sizeof(header_value));
					}

					break;

				case E_HEADER_NAME:
					if (c == '\r') {
						current = E_NEW_LINE;
						end_char_count++;
					} else {
						end_char_count = 0;

						if (c == ':') {
							current = E_HEADER_NV_SPACE;
						} else if (MAX_HEADER_COUNT > line_count - 1 && HEADER_NAME_SIZE > header_name_count) {
							/* add to header buffer */
							header_name[header_name_count++] = c;
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
						end_char_count++;

						/* add header */
						if (MAX_HEADER_COUNT > line_count - 1) {
							strncpy(req.headers[line_count - 1].name, header_name, header_name_count);
							strncpy(req.headers[line_count - 1].value, header_value, header_value_count);
							header_name_count = 0;
							header_value_count = 0;
	
							req.hsize++;
						}
					} else if (MAX_HEADER_COUNT > line_count - 1 && HEADER_VALUE_SIZE > header_value_count) {
						end_char_count = 0;

						/* add to header value buffer */
						header_value[header_value_count++] = c;
					} else {
						/* TODO: too big, handle */
					}

					break;

				default:
					break;
			}
			
			char_count++;
		}
	} while (size > 0 && 4 > end_char_count); /* 4 end characters in a row is the header-body separator of request */

	/* set method */
	if (strncmp(method, "GET", 3) == 0) req.method = M_GET;
	else if (strncmp(method, "PUT", 3) == 0) req.method = M_PUT;
	else if (strncmp(method, "DELETE", 6) == 0) req.method = M_DELETE;
	/* TODO: else error */

	/* set http version */
	if (strncmp(http_version, "HTTP/0.9", 8) == 0) req.http_version = V_09;
	else if (strncmp(http_version, "HTTP/1.0", 8) == 0) req.http_version = V_10;
	else if (strncmp(http_version, "HTTP/1.1", 8) == 0) req.http_version = V_11;
	/* TODO: else error */

	/* set path */
	strncpy(req.path, path, req.psize);

	return req;
}

void on_signal(int signal)
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
	socklen_t client_address_length;
	struct sockaddr_in server_address;
	struct sockaddr client_address;

	/* set running state */
	running = 1;

	/* set signal callback */
	signal(SIGINT, on_signal);

	/* create socket */    
	if ((sfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		fprintf(stderr, "httpfs: can't create socket\n");
		exit(-1);
	}

	/* bind to address */
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = INADDR_ANY;
	server_address.sin_port = htons(HOST_PORT);

	if (bind(sfd, (struct sockaddr*) &server_address, sizeof(server_address)) < 0) {
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
		if ((cfd = accept(sfd, &client_address, &client_address_length)) < 0 && running) { /* only show warning message when not being shut down */
			/* could not accept connection */
			fprintf(stderr, "httpfs: warn: could not accept connection\n");
			continue;
		}
		
		/* handle incoming connection */
		/* parse it */
		struct request_t req = parse_request(cfd, client_address);

		/* printreq(req); */

		/* route it & send back response */
		switch (req.method) {
			case M_GET:
				handle_get_request(cfd, req);
				break;

			case M_PUT:
				handle_put_request(cfd, req);
				break;

			case M_DELETE:
				handle_delete_request(cfd, req);
				break;

			default:
				send_response_with_content(cfd, "405", "Method Not Allowed", "text/html", "Method not allowed; only GET, PUT, and DELETE are allowed.");
				break;

		}

		/* close client */
		close(cfd);
		shutdown(cfd, SHUT_RDWR);
	}

	return 0;
}
