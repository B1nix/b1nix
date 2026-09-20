/* SPDX-License-Identifier: GPL-2.0-only */
int main(void) {
  write(2, "stderr smoke\n", 13);
  return 37;
}
