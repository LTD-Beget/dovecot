#ifndef __DICT_SERVER_H
#define __DICT_SERVER_H

struct dict;

struct dict_server *dict_server_init(const char *path);
void dict_server_deinit(struct dict_server *server);

#endif
