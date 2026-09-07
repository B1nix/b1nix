/* Exactly what libinput does to decide a device is usable. */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <libevdev-1.0/libevdev/libevdev.h>
int main(int c,char**v){
  const char*p=c>1?v[1]:"/dev/input/event1";
  int fd=open(p,O_RDWR|O_NONBLOCK);
  if(fd<0){printf("open %s fail\n",p);return 1;}
  struct libevdev*d=0;
  int r=libevdev_new_from_fd(fd,&d);
  printf("libevdev_new_from_fd(%s)=%d\n",p,r);
  if(r<0){close(fd);return 2;}
  printf(" name=%s\n",libevdev_get_name(d));
  printf(" bus=%04x vnd=%04x prd=%04x\n",libevdev_get_id_bustype(d),libevdev_get_id_vendor(d),libevdev_get_id_product(d));
  printf(" EV_KEY=%d EV_REL=%d EV_ABS=%d\n",libevdev_has_event_type(d,1),libevdev_has_event_type(d,2),libevdev_has_event_type(d,3));
  printf(" REL_X=%d REL_Y=%d BTN_LEFT=%d\n",libevdev_has_event_code(d,2,0),libevdev_has_event_code(d,2,1),libevdev_has_event_code(d,1,0x110));
  int sc=libevdev_set_clock_id(d,1);
  printf(" set_clock_id(MONOTONIC)=%d\n",sc);
  int gr=libevdev_grab(d,LIBEVDEV_GRAB);
  printf(" grab=%d\n",gr);
  libevdev_grab(d,LIBEVDEV_UNGRAB);
  libevdev_free(d); close(fd); return 0;
}
