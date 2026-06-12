#include <string.h>
#include <unistd.h>

int main(void)
{
  const char *message = "M30-DYN: ok shared-libc\n";
  size_t length = strlen(message);
  if (length != sizeof("M30-DYN: ok shared-libc\n") - 1)
    return 1;
  return write(1, message, length) == (ssize_t)length ? 0 : 1;
}
