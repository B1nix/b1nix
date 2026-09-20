int main(void) {
  int fd;
  fd = open("/tmp/b1cc-file-write.out", 577, 420);
  write(fd, "file smoke\n", 11);
  close(fd);
  return 0;
}
