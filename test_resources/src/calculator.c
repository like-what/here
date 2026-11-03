#include "io.h"
#include "power.h"

#define LINE_MAX 80

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static int read_line(char *buf, int cap) {
    int n = 0;
    while (n < cap - 1) {
        char c = (char)uart_getc();
        if (c == '\n') {
            break;
        }
        buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

static const char *skip_space(const char *s) {
    while (is_space(*s)) {
        s++;
    }
    return s;
}

static int parse_number(const char **ps, long long *out) {
    const char *s = skip_space(*ps);
    int sign = 1;

    if (*s == '+' || *s == '-') {
        if (*s == '-') {
            sign = -1;
        }
        s++;
    }
    if (*s < '0' || *s > '9') {
        return 0;
    }

    long long value = 0;
    while (*s >= '0' && *s <= '9') {
        value = value * 10 + (*s - '0');
        s++;
    }

    *out = sign < 0 ? -value : value;
    *ps = s;
    return 1;
}

static int eval_line(const char *line, long long *result) {
    const char *s = line;
    long long lhs;
    long long rhs;

    if (!parse_number(&s, &lhs)) {
        return 0;
    }
    s = skip_space(s);
    char op = *s;
    if (op != '+' && op != '-' && op != '*' && op != '/') {
        return 0;
    }
    s++;
    if (!parse_number(&s, &rhs)) {
        return 0;
    }
    s = skip_space(s);
    if (*s != '\0') {
        return 0;
    }
    if (op == '/' && rhs == 0) {
        return -1;
    }

    switch (op) {
    case '+':
        *result = lhs + rhs;
        break;
    case '-':
        *result = lhs - rhs;
        break;
    case '*':
        *result = lhs * rhs;
        break;
    case '/':
        *result = lhs / rhs;
        break;
    }
    return 1;
}

static int is_quit(const char *line) {
    const char *s = skip_space(line);
    if (s[0] == 'q' && s[1] == '\0') {
        return 1;
    }
    if (s[0] == 'q' && s[1] == 'u' && s[2] == 'i' && s[3] == 't' && skip_space(s + 4)[0] == '\0') {
        return 1;
    }
    return 0;
}

int main(void) {
    char line[LINE_MAX];

    printf("HERE bare-metal calculator\n");
    printf("Input: <number> <op> <number>\n");
    printf("ops: +  -  *  /      quit: q\n");

    for (;;) {
        printf("> ");
        read_line(line, LINE_MAX);
        if (line[0] == '\0') {
            continue;
        }
        if (is_quit(line)) {
            printf("bye\n");
            PowerOff();
        }

        long long result = 0;
        int status = eval_line(line, &result);
        if (status == 1) {
            printf("= %lld\n", result);
        } else if (status == -1) {
            printf("division by zero\n");
        } else {
            printf("bad expression\n");
        }
    }
}
