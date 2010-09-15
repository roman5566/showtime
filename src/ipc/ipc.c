#include "config.h"
#include "ipc.h"

void
ipc_init(void)
{
#ifdef CONFIG_DBUS
  dbus_start();
#endif

#ifdef CONFIG_LIRC
  lirc_start();
#endif

#ifdef CONFIG_SERDEV
  serdev_start();
#endif

#ifdef CONFIG_STDIN
  extern int listen_on_stdin;
  if(listen_on_stdin)
    stdin_start();
#endif
}
