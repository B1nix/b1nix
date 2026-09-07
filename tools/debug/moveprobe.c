/* Inject relative mouse motion into /dev/input/event1 (Linux 24-byte events). */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
struct iev { long sec, usec; unsigned short type, code; int value; };
static void ev(int fd, unsigned short t, unsigned short c, int v){struct iev e={0,0,t,c,v}; write(fd,&e,sizeof e);}
int main(int c,char**v){
  int fd=open("/dev/input/event1",O_WRONLY);
  if(fd<0){printf("open fail\n");return 1;}
  for(int i=0;i<40;i++){ ev(fd,2,0,12); ev(fd,2,1,8); ev(fd,0,0,0); usleep(30000); }
  /* a left click */
  ev(fd,1,0x110,1); ev(fd,0,0,0); usleep(50000); ev(fd,1,0x110,0); ev(fd,0,0,0);
  printf("injected 40 moves + click\n"); close(fd); return 0;
}
