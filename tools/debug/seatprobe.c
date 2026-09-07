#include <systemd/sd-bus.h>
#include <stdio.h>
int main(int c,char**v){
  const char*sid=c>1?v[1]:"c1"; sd_bus*bus=0;
  if(sd_bus_open_system(&bus)<0)return 1;
  sd_bus_error e=SD_BUS_ERROR_NULL; sd_bus_message*m=0; char path[128];
  if(sd_bus_call_method(bus,"org.freedesktop.login1","/org/freedesktop/login1","org.freedesktop.login1.Manager","GetSession",&e,&m,"s",sid)<0){printf("GetSession err=%s\n",e.message);return 2;}
  const char*p=0; sd_bus_message_read(m,"o",&p); snprintf(path,sizeof path,"%s",p); sd_bus_message_unref(m);m=0;sd_bus_error_free(&e);e=(sd_bus_error)SD_BUS_ERROR_NULL;
  /* Seat property = (so): (seat_id, object_path) -- what KWin reads */
  const char*sname=0,*sobj=0;
  if(sd_bus_get_property(bus,"org.freedesktop.login1",path,"org.freedesktop.login1.Session","Seat",&e,&m,"(so)")<0){printf("Seat prop err=%s\n",e.message);}
  else{sd_bus_message_read(m,"(so)",&sname,&sobj);printf("Seat=[%s] obj=[%s]\n",sname?sname:"(nil)",sobj?sobj:"(nil)");sd_bus_message_unref(m);m=0;}
  sd_bus_error_free(&e);e=(sd_bus_error)SD_BUS_ERROR_NULL;
  char*act=0; if(sd_bus_get_property_trivial(bus,"org.freedesktop.login1",path,"org.freedesktop.login1.Session","Active",&e,'b',&act)>=0)printf("Active=%d\n",(int)(long)act);
  unsigned vt=0; sd_bus_error_free(&e);e=(sd_bus_error)SD_BUS_ERROR_NULL;
  if(sd_bus_get_property_trivial(bus,"org.freedesktop.login1",path,"org.freedesktop.login1.Session","VTNr",&e,'u',&vt)>=0)printf("VTNr=%u\n",vt);
  return 0;
}
