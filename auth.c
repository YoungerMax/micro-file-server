/*
 * This file holds the authentication backends that can be used by the file server.
 * You can create your own with your own logic.
 * - function takes in a const char* username (1st param) and a const char* password (2nd param)
 * - function returns an int
 *   - return 0 for FALSE, meaning not authenticated OR
 *	- return 1 for TRUE, meaning authenticated successfully
 */

#ifdef __linux__

#include <pwd.h>
#include <shadow.h>
#include <unistd.h>
#include <string.h>

int auth_backend_platform_linux_shadow(const char* username, const char* password)
{
	struct passwd* pw;
	struct spwd* spw;

	pw = getpwnam(username);
	endpwent();


	if (!pw) return -1;

	spw = getspnam(pw->pw_name);
	endspent();

	char* correct_password = spw ? spw->sp_pwdp : pw->pw_passwd; 
	char* encrypted = crypt(password, correct_password);

	return strcmp(encrypted, correct_password) == 0 ? 1 : 0;
}

/* TODO

int auth_backend_platform_linux_pam(const char* username, const char* password)
{
}
*/

#endif

int auth_backend_constant_username_password(const char* username, const char* password)
{
	return strcmp(username, "super secret username") == 0 && strcmp(password, "you would never guess this password") == 0 ? 1 : 0;
}

/*
 * You can change the current authentication backend here.
 * Just change the value of this macro to whatever backend you want.
 */
#define auth_backend auth_backend_constant_username_password
