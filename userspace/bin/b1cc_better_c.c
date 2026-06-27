#include <stdio.h>
#include <stdlib.h>

typedef int my_int;
enum { VAL_A = 10, VAL_B = 20 };

struct TestStruct {
  int x;
  int y;
};

int main(void) {
  my_int a = VAL_B;
  int b = (int)a;
  
  struct TestStruct s;
  s.x = 15;
  s.y = 25;
  
  int arr[3] = {100, 200, 300};
  arr[1] = 400;
  
  int sum = b + s.x + s.y + arr[0] + arr[1] + arr[2];
  
  if (sum == 860 && VAL_A == 10) {
    puts("B1CC-BETTER-C-SMOKE: ok");
    return 0;
  } else {
    puts("B1CC-BETTER-C-SMOKE: fail");
    return 1;
  }
}
