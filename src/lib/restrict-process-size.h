#ifndef __RESTRICT_PROCESS_SIZE_H
#define __RESTRICT_PROCESS_SIZE_H

/* Restrict max. process size. The size is in megabytes, setting it to
   (unsigned int)-1 sets it unlimited. */
void restrict_process_size(unsigned int size);

#endif
