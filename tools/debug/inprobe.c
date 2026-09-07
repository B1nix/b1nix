#include <libinput.h>
#include <libudev.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
static int op(const char*p,int f,void*u){(void)u;return open(p,f);}
static void cl(int fd,void*u){(void)u;close(fd);}
static const struct libinput_interface IF={.open_restricted=op,.close_restricted=cl};
int main(int c,char**v){
  struct udev*ud=udev_new();
  struct libinput*li=libinput_udev_create_context(&IF,NULL,ud);
  if(!li)return 1;
  if(libinput_udev_assign_seat(li,c>1?v[1]:"seat0"))return 2;
  libinput_dispatch(li);
  int n=0;struct libinput_event*e;
  while((e=libinput_get_event(li))){
    if(libinput_event_get_type(e)==LIBINPUT_EVENT_DEVICE_ADDED){
      struct libinput_device*d=libinput_event_get_device(e);
      fprintf(stderr,"DEVICE_ADDED: %s ptr=%d kbd=%d\n",libinput_device_get_name(d),
        libinput_device_has_capability(d,LIBINPUT_DEVICE_CAP_POINTER),
        libinput_device_has_capability(d,LIBINPUT_DEVICE_CAP_KEYBOARD));n++;}
    libinput_event_destroy(e);}
  fprintf(stderr,"libinput devices: %d\n",n);return 0;
}
