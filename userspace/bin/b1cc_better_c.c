#include <stdio.h>
#include <stdlib.h>

typedef int my_int;
enum { VAL_A = 10, VAL_B = 20 };

struct TestStruct {
  int x;
  int y;
};

int my_global_var = 123;

int main(void) {
  my_int a = VAL_B;
  int b = (int)a;
  
  struct TestStruct s;
  s.x = 15;
  s.y = 25;
  
  int arr[3] = {100, 200, 300};
  arr[1] = 400;
  
  int sum = b + s.x + s.y + arr[0] + arr[1] + arr[2];
  
  int test_var = 5;
  test_var++;
  test_var += 4;
  
  int switch_res = 0;
  switch (test_var) {
    case 10:
      switch_res = 42;
      break;
    default:
      switch_res = 0;
      break;
  }
  
  if (sum == 860 && VAL_A == 10 && (10 & 6) == 2 && (2 << 3) == 16 && test_var == 10 && switch_res == 42 && my_global_var == 123) {
    puts("B1CC-BETTER-C-SMOKE: ok");
    return 0;
  } else {
    puts("B1CC-BETTER-C-SMOKE: fail");
    return 1;
  }
}
