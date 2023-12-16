#!/bin/bash
gcc -Wall -Wextra myshell.c -L. -l:libparser.a -o myshell -static

chmod u+x myshell
