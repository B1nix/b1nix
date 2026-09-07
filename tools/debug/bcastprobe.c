/* Does a write to event1 broadcast to another reader of event1? */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
struct iev { long sec, usec; unsigned short type, code; int value; };
int main(void){
  int rd=open("/dev/input/event1",O_RDONLY|O_NONBLOCK);
  int wr=open("/dev/input/event1",O_WRONLY);
  if(rd<0||wr<0){printf("open rd=%d wr=%d\n",rd,wr);return 1;}
  struct iev e={0,0,2,0,15}; write(wr,&e,sizeof e);
  e.type=2;e.code=1;e.value=9; write(wr,&e,sizeof e);
  e.type=0;e.code=0;e.value=0; write(wr,&e,sizeof e);
  usleep(100000);
  struct iev r; int n=0;
  while(read(rd,&r,sizeof r)==(int)sizeof r){printf("read type=%u code=%u val=%d\n",r.type,r.code,r.value);n++;}
  printf("reader got %d events\n",n);
  close(rd);close(wr);return 0;
}
