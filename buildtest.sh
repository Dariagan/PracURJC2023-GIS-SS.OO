#!bin/bash
gcc -Wall -Wextra test.c -L. -l:libparser.a -o test -static

chmod u+x test