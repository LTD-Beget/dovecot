#ifndef __BASE64_H
#define __BASE64_H

/* Translates binary data into base64. Allocates memory from temporary pool. */
const char *base64_encode(const unsigned char *data, size_t size);

/* Translates base64 data into binary modifying the data itself.
   Returns size of the binary data, or -1 if error occured. */
ssize_t base64_decode(char *data);

#endif
