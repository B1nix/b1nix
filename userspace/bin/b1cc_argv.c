int main(int argc, char **argv) {
  if (argc < 2) return 1;
  write(1, argv[1], strlen(argv[1]));
  write(1, "\n", 1);
  return 0;
}
