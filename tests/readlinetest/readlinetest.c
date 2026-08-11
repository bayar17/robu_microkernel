#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/readline.h>
#include <readline/history.h>

int main(void) {
    printf("readlinetest: starting\n");

    char *line = readline("readlinetest> ");
    if (!line) {
        printf("readlinetest: readline() returned NULL\n");
        return 1;
    }
    printf("readlinetest: got line: \"%s\"\n", line);

    add_history(line);
    HIST_ENTRY *h = history_get(history_base);
    printf("readlinetest: history[0]=\"%s\"\n", h ? h->line : "(null)");

    free(line);
    printf("readlinetest: done\n");
    return 0;
}
