/* Hold one bus connection: TakeControl then TakeDevice, like kwin/libinput. */
#include <systemd/sd-bus.h>
#include <stdio.h>
#include <string.h>
int main(int c,char**v){
  const char*sid=c>1?v[1]:"c1";
  sd_bus*bus=0; int r=sd_bus_open_system(&bus);
  if(r<0){printf("open_system %d\n",r);return 1;}
  sd_bus_error e=SD_BUS_ERROR_NULL; sd_bus_message*m=0;
  char path[128];
  r=sd_bus_call_method(bus,"org.freedesktop.login1","/org/freedesktop/login1",
    "org.freedesktop.login1.Manager","GetSession",&e,&m,"s",sid);
  if(r<0){printf("GetSession err=%s\n",e.message);return 2;}
  const char*p=0; sd_bus_message_read(m,"o",&p); strncpy(path,p,sizeof path-1);
  printf("session path=%s\n",path); sd_bus_message_unref(m); m=0; sd_bus_error_free(&e); e=(sd_bus_error)SD_BUS_ERROR_NULL;
  r=sd_bus_call_method(bus,"org.freedesktop.login1",path,
    "org.freedesktop.login1.Session","TakeControl",&e,&m,"b",0);
  printf("TakeControl r=%d err=%s\n",r,r<0?e.message:"ok"); sd_bus_message_unref(m);m=0;
  unsigned mm[3][2]={{13,65},{13,64},{226,1}};
  for(int i=0;i<3;i++){sd_bus_error_free(&e);e=(sd_bus_error)SD_BUS_ERROR_NULL;
    r=sd_bus_call_method(bus,"org.freedesktop.login1",path,
      "org.freedesktop.login1.Session","TakeDevice",&e,&m,"uu",mm[i][0],mm[i][1]);
    if(r<0){printf("TakeDevice(%u:%u) ERR=%s\n",mm[i][0],mm[i][1],e.message);}
    else{int fd=-1;int paused=0;r=sd_bus_message_read(m,"hb",&fd,&paused);
      printf("TakeDevice(%u:%u) OK fd=%d paused=%d\n",mm[i][0],mm[i][1],fd,paused);sd_bus_message_unref(m);m=0;}
  }
  return 0;
}
