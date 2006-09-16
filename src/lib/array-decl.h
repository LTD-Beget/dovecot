#ifndef __ARRAY_DECL_H
#define __ARRAY_DECL_H

#define ARRAY_DEFINE(name, array_type) union { struct array arr; array_type const *const *v; array_type **v_modifiable; } name
#define ARRAY_INIT { { 0, 0 } }

#define ARRAY_DEFINE_TYPE(name, array_type) \
	union array ## __ ## name { struct array arr; array_type const *const *v; array_type **v_modifiable; }
#define ARRAY_TYPE(name) \
	union array ## __ ## name

struct array {
	buffer_t *buffer;
	size_t element_size;
};

#endif
