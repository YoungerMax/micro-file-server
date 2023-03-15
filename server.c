#include <dirent.h>
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

#include "auth.c"


#define HOST_PORT 8081
#define BACKLOG 10
#define BUFFER_SIZE 1024
#define PATH_BUFFER_SIZE 512
#define HEADER_NAME_SIZE 48
#define HEADER_VALUE_SIZE 1024
#define MAX_HEADER_COUNT 64
#define SERVER_NAME "micro"

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

enum parse_error
{
	ERR_UNSUPPORTED_HTTP_VERSION,
	ERR_UNSUPPORTED_METHOD,
	ERR_METHOD_TOO_BIG,
	ERR_PATH_TOO_BIG,
	ERR_TOO_MANY_HEADERS,
	ERR_HTTP_VERSION_TOO_BIG,
	ERR_HEADER_NAME_TOO_BIG,
	ERR_HEADER_VALUE_TOO_BIG,
	ERR_EXPECTED_NEW_LINE,
	ERR_EXPECTED_NAME_VALUE_SPACE,
	ERR_EXPECTING_UNKNOWN /* this REALLY shouldn't happen, this is internal */
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

const unsigned int FROM_BASE64[] = {
    80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80,
    80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 80, 62, 80, 80, 80, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 80, 80, 80, 64, 80, 80,
    80,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 80, 80, 80, 80, 80,
    80, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 80, 80, 80, 80, 80
};

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
	.value = SERVER_NAME
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

	return ""; 
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

	return "";
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
	char* response_buffer = malloc(get_response_length(response));

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
	/* length of contentlen as a string */
	long length_of_content_length = content_length == 0 ? 1 : (long) 1 + log10((double) content_length) + 1;
	char* content_length_buffer = malloc(length_of_content_length);
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

void send_response_basic(int cfd, const char status_code[STATUS_CODE_SIZE], const char* status_text)
{
	struct response_t response = {
		.headers = {
			H_SERVER,
			H_CONNECTION_CLOSED
		},
		.header_count = 2,
		.http_version = V_11,
		.status_text = status_text,
		.status_code = status_code
	};

	send_response(cfd, response);
}

void send_response_with_content(int cfd, const char status_code[STATUS_CODE_SIZE], const char* status_text, const char* content_type, const char* content)
{
	send_response_with_content_length(cfd, status_code, status_text, content_type, strlen(content));
	send(cfd, content, strlen(content), 0);
}

void send_http_file(int cfd, const char* file_path, size_t file_size)
{
	size_t read_length;
	int fd;
	char file_send_buffer[BUFFER_SIZE];

	/* open file */
	if ((fd = open(file_path, O_RDONLY)) < 0) {
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
	send_response_basic(cfd, "404", "Not Found");
}

void send_directory_entry(int cfd, const struct dirent* entry)
{
	size_t entry_length = strlen(entry->d_name);

	if (entry->d_type == DT_DIR) entry_length++;

	size_t buffer_length = DIRLEN(entry_length);
	char* send_buffer = malloc(buffer_length);

	/* send entry as link */
	if (entry->d_type == DT_DIR) {
		snprintf(send_buffer, buffer_length, "<a href=\"%s/\">%s/</a><br>", entry->d_name, entry->d_name);
	} else {
		snprintf(send_buffer, buffer_length, "<a href=\"%s\">%s</a><br>", entry->d_name, entry->d_name);
	}

	send(cfd, send_buffer, buffer_length - 1, 0); /* don't send the \0 */
	free(send_buffer);
}

/* special thanks to http://www.sunshine2k.de/articles/coding/base64/understanding_base64.html
 * great explaination of base64
 * original C# algorithm
 * below is my version adapted for C
 */
char* base64_decode(const char* str)
{
	size_t len, i, actual_len;
	char byte_1, byte_2, byte_3, byte_4;
	size_t counter = 0;
	char* out_str;

	len = strlen(str);

	/* all base64 strings must be divisible by 4 */
	if (len % 4 != 0)
		return NULL;
	
	actual_len = len / 4 * 3 + 1; /* +1 for null byte */
	out_str = malloc(actual_len);

	for (i = 0; len > i; i += 4) {
		byte_1 = FROM_BASE64[(unsigned int) str[i]];
		byte_2 = FROM_BASE64[(unsigned int) str[i + 1]];
		byte_3 = FROM_BASE64[(unsigned int) str[i + 2]];
		byte_4 = FROM_BASE64[(unsigned int) str[i + 3]];

		if (str[i + 3] == '=') {	
			if (str[i + 2] == '=') {
				out_str[counter++] = byte_1 << 2 | ((byte_2 & 0xf0) >> 4);
			} else {
				out_str[counter++] = byte_1 << 2 | ((byte_2 & 0xf0) >> 4);
				out_str[counter++] = ((byte_2 & 0x0f) << 4) | ((byte_3 & 0x3c) >> 2);
			}
		} else {
			out_str[counter++] = byte_1 << 2 | ((byte_2 & 0xf0) >> 4);
			out_str[counter++] = ((byte_2 & 0x0f) << 4) | ((byte_3 & 0x3c) >> 2);
			out_str[counter++] = ((byte_3 & 0x03) << 6) | (byte_4 & 0x3f);
		}
	}

	/* need null byte */
	out_str[counter++] = '\0';

	return out_str;
}

int is_authenticated_http(struct request_t req)
{
	int header_index;
	char authenticated;
	char* decoded;

	if (!(header_index = get_header_index(req, "Authorization"))) {
		return -1;
	}

	struct header_t authorization = req.headers[header_index];
	
	const char* schema = strtok(authorization.value, " ");
	const char* encoded = strtok(NULL, " ");

	if (schema == NULL || encoded == NULL)
		return -2;

	decoded = base64_decode(encoded);  /* [!!] this allocates, MUST free */

	if (decoded == NULL) {
		free(decoded);
		return -3;
	}

	const char* username = strtok(decoded, ":");
	const char* password = strtok(NULL, ":");

	if (username == NULL || password == NULL) {
		free(decoded);
		return -4;
	}

	authenticated = auth_backend(username, password);

	free(decoded);

	return authenticated;
}

void send_directory_listing(int cfd, const char* directory_path)
{

	DIR* dir;
	struct dirent* entry;
	size_t size = 0, name_length;

	/* open directory */
	if ((dir = opendir(directory_path)) == NULL) {
		/* not found */
		send_not_found(cfd);

		return;
	}

	/* count for content length */
	while ((entry = readdir(dir)) != NULL) {
		name_length = strlen(entry->d_name);

		if (entry->d_type == DT_DIR) name_length++; /* add for the "/" */

		size += DIRLEN(name_length) - 1;
	}
	
	/* start http response */
	send_response_with_content_length(cfd, "200", "OK", "text/html", size);
	rewinddir(dir);

	/* iterate through directory and send as HTML */
	while ((entry = readdir(dir)) != NULL)
		send_directory_entry(cfd, entry);
		
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
	long read_bytes = 0, content_length;
	char buffer[BUFFER_SIZE];

	/* must be authenticated */
	if (1 > is_authenticated_http(req)) {
		send_response_basic(cfd, "401", "Unauthorized");
		return;
	}

	/* TODO: add sufficient space check */

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
		send_response_with_content(cfd, "411", "Length Required", "text/html", "Expected Content-Length header");
		return;
	}
	
	content_length = atol(req.headers[header_index].value);

	/* get expect header */
	if ((header_index = get_header_index(req, "Expect")) != -1) {
		/* only directive is `100-continue` */
		send_response_basic(cfd, "100", "Continue");
	}

	/* write to filesystem */
	while ((read_length = recv(cfd, buffer, BUFFER_SIZE, 0)) > 0) {
		read_bytes += read_length;
		write(fd, buffer, read_length);

		if (read_bytes >= content_length) break;
	}

	send_response_basic(cfd, "201", "Created");

	close(fd);
}

void handle_delete_request(int cfd, struct request_t req)
{
	/* must be authenticated */
	if (1 > is_authenticated_http(req)) {
		send_response_basic(cfd, "401", "Unauthorized");
		return;
	}

	/* TODO: add file exists check */
	/* TODO: add file is file and not directory check */

	if (remove(req.path)) {
		send_response_basic(cfd, "500", "Internal Server Error");
		return;
	}

	send_response_basic(cfd, "204", "No Content");
}

enum parse_error parse_request(int cfd, struct sockaddr client_address, struct request_t* request)
{
	ssize_t size;
	char buffer[BUFFER_SIZE];
	char method[METHOD_BUFFER_SIZE], path[PATH_BUFFER_SIZE], http_version[HTTP_VERSION_SIZE], header_name[HEADER_NAME_SIZE], header_value[HEADER_VALUE_SIZE];
	int i, char_count = 0, end_char_count = 0, line_count = 0, header_name_count = 0, header_value_count = 0;
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
						return ERR_METHOD_TOO_BIG;
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
						return ERR_PATH_TOO_BIG;
					}

					break;

				case E_HTTP_VER:
					if (c == '\r') {
						current = E_NEW_LINE;
						end_char_count++;
					} else if (HTTP_VERSION_SIZE > char_count) {
						http_version[char_count] = c;
					} else {
						return ERR_HTTP_VERSION_TOO_BIG;
					}

					break;

				case E_NEW_LINE:
					if (c == '\n') {
						current = E_HEADER_NAME;
						end_char_count++;
						line_count++;

						memset(header_name, 0, sizeof(header_name));
						memset(header_value, 0, sizeof(header_value));
					} else {
						/* expected new line, error */
						return ERR_EXPECTED_NEW_LINE;
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
							return ERR_HEADER_NAME_TOO_BIG;
						}
					}

					break;

				case E_HEADER_NV_SPACE:
					if (c == ' ') {
						current = E_HEADER_VAL;
					} else {
						/* expected space */
						return ERR_EXPECTED_NAME_VALUE_SPACE;
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
						} else {
							/* error: too many headers! */
							return ERR_TOO_MANY_HEADERS;
						}
					} else if (MAX_HEADER_COUNT > line_count - 1 && HEADER_VALUE_SIZE > header_value_count) {
						end_char_count = 0;

						/* add to header value buffer */
						header_value[header_value_count++] = c;
					} else {
						return ERR_HEADER_VALUE_TOO_BIG;
					}

					break;

				default:
					return ERR_EXPECTING_UNKNOWN;
			}
			
			char_count++;
		}
	} while (size > 0 && 4 > end_char_count); /* 4 end characters in a row is the header-body separator of request */

	/* set method */
	if (strncmp(method, "GET", 3) == 0) {
		req.method = M_GET;
	} else if (strncmp(method, "PUT", 3) == 0) {
		req.method = M_PUT;
	} else if (strncmp(method, "DELETE", 6) == 0) {
		req.method = M_DELETE;
	} else {
		return ERR_UNSUPPORTED_METHOD;
	}

	/* set http version */
	if (strncmp(http_version, "HTTP/0.9", 8) == 0) {
		req.http_version = V_09;
	} else if (strncmp(http_version, "HTTP/1.0", 8) == 0) {
		req.http_version = V_10;
	} else if (strncmp(http_version, "HTTP/1.1", 8) == 0) {
		req.http_version = V_11;
	} else {
		return ERR_UNSUPPORTED_HTTP_VERSION;
	}

	/* set path */
	strncpy(req.path, path, req.psize);

	/* copy local request into the passed in pointer */
	memcpy(request, &req, sizeof(req));

	return 0;
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
		fprintf(stderr, SERVER_NAME": can't create socket\n");
		exit(-1);
	}

	/* allow rebinding of socket, need this as a constant value to pass as pointer */
	const int ALLOW = 1;

	if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void*) &ALLOW, sizeof(ALLOW)) < 0)
		fprintf(stderr, SERVER_NAME": warn: can't set SO_REUSEADDR\n");

	if (setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, (void*) &ALLOW, sizeof(ALLOW)) < 0) 
		fprintf(stderr, SERVER_NAME": warn: can't set SO_REUSEPORT\n");

	/* bind to address */
	server_address.sin_family = AF_INET;
	server_address.sin_addr.s_addr = INADDR_ANY;
	server_address.sin_port = htons(HOST_PORT);

	if (bind(sfd, (struct sockaddr*) &server_address, sizeof(server_address)) < 0) {
		fprintf(stderr, SERVER_NAME": can't bind to port %i\n", HOST_PORT);
		exit(-2);
	}

	/* listen */
	if (listen(sfd, BACKLOG) < 0) {
		fprintf(stderr, SERVER_NAME": could not start listening on port %i\n", HOST_PORT);
		exit(-3);
	}

	/* process loop */
	while (running) {
		if ((cfd = accept(sfd, &client_address, &client_address_length)) < 0 && running) { /* only show warning message when not being shut down */
			/* could not accept connection */
			fprintf(stderr, SERVER_NAME": warn: could not accept connection\n");
			continue;
		}
		
		/* handle incoming connection */
		/* parse it */
		struct request_t req;
		enum parse_error parse_error;
		
		if ((parse_error = parse_request(cfd, client_address, &req))) {
			switch (parse_error) {
				case ERR_UNSUPPORTED_HTTP_VERSION:
				case ERR_HTTP_VERSION_TOO_BIG:
					send_response_basic(cfd, "505", "HTTP Version Not Supported");
					break;

				case ERR_UNSUPPORTED_METHOD:
				case ERR_METHOD_TOO_BIG:
					send_response_basic(cfd, "405", "Method Not Allowed");
					break;

				case ERR_EXPECTING_UNKNOWN:
					send_response_basic(cfd, "500", "Internal Server Error");
					break;

				case ERR_TOO_MANY_HEADERS:
				case ERR_HEADER_VALUE_TOO_BIG:
				case ERR_HEADER_NAME_TOO_BIG:
					send_response_basic(cfd, "431", "Request Header Fields Too Large");
					break;
				
				case ERR_EXPECTED_NAME_VALUE_SPACE:
				case ERR_EXPECTED_NEW_LINE:
					send_response_basic(cfd, "400", "Bad Request");
					break;

				case ERR_PATH_TOO_BIG:
					send_response_basic(cfd, "414", "Request-URI Too Long");
					break;
			}
		} else {
			print_request(req);
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

			}
		}

		/* close client */
		close(cfd);
		shutdown(cfd, SHUT_RDWR);
	}

	return 0;
}
