#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int a[1000000];
int b[1000000];
int c[1000000];
int
main() {
  struct timeval begin, end;
  double elapsed;
  gettimeofday(&begin, NULL);
  printf("hello world\n");
  //int k[100][100];
  int i;
  for (i=0; i<1000000; i++) {
    a[i] = 12;
  }
  char *name = (char *)malloc(sizeof(char) * 8);
  name = "ankit";
  printf("Name: %s\n", name);
  gettimeofday(&end, NULL);
  elapsed = (end.tv_sec - begin.tv_sec) + ((end.tv_usec - begin.tv_usec)/1000000.0);
  printf("Time Elapsed: %f\n", elapsed);
 // sleep(15);
  return 0;
}
