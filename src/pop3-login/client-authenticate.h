#ifndef __CLIENT_AUTHENTICATE_H
#define __CLIENT_AUTHENTICATE_H

int cmd_capa(struct pop3_client *client, const char *args);
int cmd_user(struct pop3_client *client, const char *args);
int cmd_pass(struct pop3_client *client, const char *args);
int cmd_auth(struct pop3_client *client, const char *args);
int cmd_apop(struct pop3_client *client, const char *args);

#endif
