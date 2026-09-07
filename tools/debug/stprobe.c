#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
int main(int c,char**v){
  const char*p=c>1?v[1]:"/dev/input/event1";
  struct stat sp; int r1=stat(p,&sp);
  int fd=open(p,O_RDWR|O_NONBLOCK);
  struct stat sf; int r2=fstat(fd,&sf);
  printf("stat(%s)=%d mode=%o ifmt=%o rdev=%lu:%lu\n",p,r1,(unsigned)sp.st_mode,(unsigned)(sp.st_mode&S_IFMT),
    (unsigned long)((sf.st_rdev>>8)&0xfff),(unsigned long)(sp.st_rdev&0xff));
  printf("fstat(fd=%d)=%d mode=%o ifmt=%o(S_IFCHR=%o) rdev=%lu:%lu\n",fd,r2,(unsigned)sf.st_mode,
    (unsigned)(sf.st_mode&S_IFMT),(unsigned)S_IFCHR,(unsigned long)((sf.st_rdev>>8)&0xfff),(unsigned long)(sf.st_rdev&0xff));
  close(fd);return 0;
}
