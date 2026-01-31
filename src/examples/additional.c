#include <stdio.h>
#include <stdlib.h>
#include "lib/user/syscall.h"

int
main (int argc, char *argv[])
{
  
  /* Convert the string arguments from the command line into integers.
     The `atoi` function (ASCII to integer) is used for this conversion. */
  int num1 = atoi(argv[1]);
  int num2 = atoi(argv[2]);
  int num3 = atoi(argv[3]);
  int num4 = atoi(argv[4]);

  /* Call the new system calls that are now part of the user library. */
  int fib_result = fibonacci(num1);
  int max_result = max_of_four_int(num1, num2, num3, num4);

  /* Print the results in the required format: "result1 result2" followed by a newline. */
  printf ("%d %d\n", fib_result, max_result);

  return EXIT_SUCCESS;
}

