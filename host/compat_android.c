/* Android filesystem operations. Sparse hole punching is optional. */
#include "compat_android.h"
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/falloc.h>
#endif
int menhir_disk_sync(int fd) { return fsync(fd); }
int menhir_disk_punch(int fd,off_t offset,off_t length) {
#ifdef __linux__
 return fallocate(fd,FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE,offset,length);
#else
 (void)fd;(void)offset;(void)length;errno=ENOTSUP;return -1;
#endif
}
static char *child(const char *parent,const char *name) { char *p=NULL;if(asprintf(&p,"%s/%s",parent,name)<0)return NULL;return p; }
int menhir_remove_tree(const char *path) {
 struct stat st;if(lstat(path,&st))return -1;
 if(!S_ISDIR(st.st_mode))return unlink(path);
 DIR *d=opendir(path);if(!d)return -1;struct dirent *e;int rc=0;
 while((e=readdir(d))) { if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;char *p=child(path,e->d_name);if(!p||menhir_remove_tree(p))rc=-1;free(p);if(rc)break; }
 closedir(d);return rc?rc:rmdir(path);
}
int menhir_copy_tree(const char *from,const char *to) {
 struct stat st;if(lstat(from,&st))return -1;
 /* Do not follow symlinks across the private share boundary. */
 if(S_ISLNK(st.st_mode)) { errno=ENOTSUP;return -1; }
 if(S_ISDIR(st.st_mode)) {
  if(mkdir(to,st.st_mode&0777)&&errno!=EEXIST)return -1;
  DIR *d=opendir(from);if(!d)return -1;struct dirent *e;int rc=0;
  while((e=readdir(d))) { if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;char *a=child(from,e->d_name),*b=child(to,e->d_name);if(!a||!b||menhir_copy_tree(a,b))rc=-1;free(a);free(b);if(rc)break; }
  closedir(d);return rc;
 }
 if(!S_ISREG(st.st_mode)) { errno=ENOTSUP;return -1; }
 int in=open(from,O_RDONLY|O_NOFOLLOW);if(in<0)return -1;
 int out=open(to,O_WRONLY|O_CREAT|O_NOFOLLOW,st.st_mode&0777);if(out<0){close(in);return -1;}
 struct stat a,b;int rc=-1;
 if(fstat(in,&a)||fstat(out,&b)||(a.st_dev==b.st_dev&&a.st_ino==b.st_ino)||ftruncate(out,0))goto done;
 char buf[65536];ssize_t n;
 while((n=read(in,buf,sizeof buf))>0) { ssize_t at=0;while(at<n){ssize_t w=write(out,buf+at,n-at);if(w<0&&errno==EINTR)continue;if(w<=0)goto done;at+=w;} }
 if(n==0)rc=0;
done:close(in);if(close(out))rc=-1;return rc;
}
