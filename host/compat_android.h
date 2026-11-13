#ifndef MENHIR_COMPAT_ANDROID_H
#define MENHIR_COMPAT_ANDROID_H
#include <stddef.h>
#include <sys/types.h>
int menhir_disk_sync(int fd);
int menhir_disk_punch(int fd,off_t offset,off_t length);
int menhir_copy_tree(const char *from,const char *to);
int menhir_remove_tree(const char *path);
#endif
