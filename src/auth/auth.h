#ifndef __AUTH_H
#define __AUTH_H

struct auth {
	struct mech_module_list *mech_modules;
	buffer_t *mech_handshake;

	struct passdb_module *passdb;
	struct userdb_module *userdb;

#ifdef HAVE_MODULES
	struct auth_module *passdb_module;
	struct auth_module *userdb_module;
#endif

	char *passdb_args, *userdb_args;

	const char *const *auth_realms;
	const char *default_realm;
	const char *anonymous_username;
	char username_chars[256];
        char username_translation[256];
	int ssl_require_client_cert;
};

const string_t *auth_mechanisms_get_list(struct auth *auth);

struct auth *auth_preinit(void);
void auth_init(struct auth *auth);
void auth_deinit(struct auth *auth);

#endif
