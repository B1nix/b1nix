/* What libudev (thus libinput) sees for the input devices. */
#include <libudev.h>
#include <stdio.h>
#include <string.h>
static const char*P(struct udev_device*d,const char*k){const char*v=udev_device_get_property_value(d,k);return v?v:"(nil)";}
int main(void){
  struct udev*u=udev_new();
  struct udev_enumerate*e=udev_enumerate_new(u);
  udev_enumerate_add_match_subsystem(e,"input");
  udev_enumerate_scan_devices(e);
  struct udev_list_entry*l,*devs=udev_enumerate_get_list_entry(e);
  int cnt=0;
  udev_list_entry_foreach(l,devs){
    const char*sp=udev_list_entry_get_name(l);
    struct udev_device*d=udev_device_new_from_syspath(u,sp);
    if(!d)continue;
    const char*node=udev_device_get_devnode(d);
    printf("SYS %s\n  node=%s subsys=%s init=%d ID_INPUT=%s MOUSE=%s KEYBOARD=%s\n",
      sp, node?node:"(none)", udev_device_get_subsystem(d),
      udev_device_get_is_initialized(d), P(d,"ID_INPUT"),P(d,"ID_INPUT_MOUSE"),P(d,"ID_INPUT_KEYBOARD"));
    struct udev_device*par=udev_device_get_parent(d);
    printf("  parent=%s devgroup=%s seat=%s\n",
      par?udev_device_get_syspath(par):"(none)", P(d,"LIBINPUT_DEVICE_GROUP"), P(d,"ID_SEAT"));
    if(node)cnt++;
    udev_device_unref(d);
  }
  printf("nodes with devnode: %d\n",cnt);
  return 0;
}
