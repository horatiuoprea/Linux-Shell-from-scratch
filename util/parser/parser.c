// C program to illustrate
// pipe system call in C
#include <stdio.h>   // For printf()
#include <stdlib.h>  // For exit()
#include <string.h>
#include <unistd.h>  // For pipe(), read(), write()
#define MSGSIZE 16

int main() {
    char inbuf[MSGSIZE];
    scanf("%s", inbuf);
    int l = strlen(inbuf);
    for (int i = 0; i < l / 2; i++) {
        char aux = inbuf[i];
        inbuf[i] = inbuf[l - i - 1];
        inbuf[l - i - 1] = aux;
    }
    printf("%s\n", inbuf);

    return 0;
}