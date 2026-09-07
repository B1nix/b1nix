/* What EVIOCG* actually returns for /dev/input/event1. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <stdint.h>
#define EVIOCGID   0x80084502
#define _EVIOCGNAME(len) (0x81004506U | ((len)<<16))
#define EVIOCGBIT(ev,len) (0x80000520U | ((ev)<<0) | ((len)<<16))
/* EVIOCGBIT(ev,len): dir=READ(2), 'E'(0x45), nr=0x20+ev, size=len */
static unsigned gbit(int ev,int len){return (2U<<30)|('E'<<8)|((0x20+ev)&0xff)|((len&0x3fff)<<16);}
static unsigned gabs(int ax,int len){return (2U<<30)|('E'<<8)|((0x40+ax)&0xff)|((len&0x3fff)<<16);}
static unsigned gid(void){return (2U<<30)|('E'<<8)|0x02|((8&0x3fff)<<16);}
static unsigned gname(int len){return (2U<<30)|('E'<<8)|0x06|((len&0x3fff)<<16);}
static void hex(const char*t,unsigned char*b,int n){printf("%s",t);for(int i=n-1;i>=0;i--)printf("%02x",b[i]);printf("\n");}
int main(int c,char**v){
  const char*p=c>1?v[1]:"/dev/input/event1";
  int fd=open(p,O_RDONLY);
  if(fd<0){printf("open %s fail\n",p);return 1;}
  unsigned char ev[8]={0},key[96]={0},rel[8]={0},abs[8]={0};
  int r;
  r=ioctl(fd,gbit(0,sizeof ev),ev);   printf("EVIOCGBIT(0) ret=%d ",r); hex("ev=",ev,8);
  r=ioctl(fd,gbit(1,sizeof key),key); printf("EVIOCGBIT(KEY) ret=%d key[0..8]=",r);for(int i=0;i<8;i++)printf("%02x",key[i]);printf(" key[0x110/8=0x22]=%02x\n",key[0x22]);
  r=ioctl(fd,gbit(2,sizeof rel),rel); printf("EVIOCGBIT(REL) ret=%d ",r); hex("rel=",rel,8);
  r=ioctl(fd,gbit(3,sizeof abs),abs); printf("EVIOCGBIT(ABS) ret=%d ",r); hex("abs=",abs,8);
  char nm[64]={0}; r=ioctl(fd,gname(sizeof nm-1),nm); printf("EVIOCGNAME ret=%d name=%s\n",r,nm);
  unsigned short id[4]={0}; r=ioctl(fd,gid(),id); printf("EVIOCGID ret=%d bus=%04x vnd=%04x prd=%04x ver=%04x\n",r,id[0],id[1],id[2],id[3]);
  close(fd); return 0;
}
