// Dumps utf8proc's view for comparison with node: per code point a flag byte (bit0 pictographic,
// bit1 mark, bit2 control), then grapheme boundaries for strings read from stdin, one per line,
// as UTF-32 hex.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utf8proc.h"

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "props") == 0) {
        for (int c = 0; c < 0x110000; c++) {
            utf8proc_category_t k = utf8proc_category(c);
            int f = (utf8proc_get_property(c)->boundclass == UTF8PROC_BOUNDCLASS_EXTENDED_PICTOGRAPHIC) |
                    ((k == UTF8PROC_CATEGORY_MN || k == UTF8PROC_CATEGORY_MC || k == UTF8PROC_CATEGORY_ME) << 1) |
                    ((k == UTF8PROC_CATEGORY_CC) << 2);
            putchar('0' + f);
        }
        return 0;
    }
    char line[1 << 16];
    while (fgets(line, sizeof line, stdin)) {
        int cps[8192], n = 0;
        for (char *p = strtok(line, " \r\n"); p; p = strtok(nullptr, " \r\n")) cps[n++] = (int)strtol(p, nullptr, 16);
        utf8proc_int32_t state = 0;
        for (int i = 0; i < n; i++) {
            if (i && utf8proc_grapheme_break_stateful(cps[i - 1], cps[i], &state)) putchar('|');
            printf("%x ", cps[i]);
        }
        putchar('\n');
    }
    return 0;
}
