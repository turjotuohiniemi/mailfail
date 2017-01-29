#include <stdio.H>
#include <stdlib.h>
#include <grp.h>
#include <uuid/uuid.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <time.h>
#include <signal.h>

#include "conf.h"

char *appname = "mailfail";
#define READ_BUF_SIZE (8192)

struct header {
    struct header *next;
    char *name;
    char *value;
    char *line;
} *all_headers, *last_header;

char *parse_header_name(char *line) {
    char *c = strtok(line, ":");
    return strdup(c);
}

pid_t child_pid;
FILE *exim_out;

void add_header(char *line) {
    if (*line == '\t' || *line == ' ') {
        if (last_header == NULL)
            return;
        char *newbuf = malloc(strlen(last_header->line) + strlen(line) + 1);
        if (newbuf == NULL) {
            perror(appname);
            exit(EXIT_FAILURE);
        }
        strcat(strcpy(newbuf, last_header->line), line);
        free(last_header->line);
        last_header->line = newbuf;
    } else {
        struct header *newhdr = malloc(sizeof(struct header));
        if (newhdr == NULL) {
            perror(appname);
            exit(EXIT_FAILURE);
        }
        if (all_headers == NULL)
            all_headers = last_header = newhdr;
        newhdr->line = strdup(line);
        newhdr->next = NULL;
        newhdr->value = NULL;
        newhdr->name = parse_header_name(line);
        last_header->next = newhdr;
        last_header = newhdr;
    }
}

char *parse_header_value(char *line) {
    char *start = strchr(line, ':') + 1;
    while (*start == ' ' || *start == '\t')
        ++start;
    char *end = strchr(start, '\n');
    char *buf = malloc(end - start + 1);
    if (buf == NULL) {
        perror(appname);
        exit(EXIT_FAILURE);
    }
    strncpy(buf, start, end - start);
    buf[end - start] = 0;
    return buf;
}

char *get_header(const char *name) {
    for (struct header *hdr = all_headers; hdr; hdr = hdr->next) {
        if (!strcasecmp(name, hdr->name)) {
            if (!hdr->value)
                hdr->value = parse_header_value(hdr->line);
            return hdr->value;
        }
    }
    return NULL;
}

char *read_line(char *buf, int maxsize) {
    if (fgets(buf, maxsize, stdin) == NULL)
        return NULL;
    if (buf[strlen(buf) - 1] != '\n') {
        fprintf(stderr, "%s: header line too long (%d)\n", appname, maxsize);
        exit(EXIT_FAILURE);
    }
    return buf;
}

void read_headers() {
    char line[READ_BUF_SIZE];
    if (read_line(line, sizeof line) == NULL) {
        fprintf(stderr, "%s: empty message without headers\n", appname);
        exit(EXIT_FAILURE);
    }
    while (line[0] != '\n' && line[0] != '\r') {
        add_header(line);
        if (read_line(line, sizeof line) == NULL) {
            fprintf(stderr, "%s: end of file before message body\n", appname);
        }
    }
}

void exec_exim() {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }
    struct group *grp;
    errno = 0;
    grp = getgrnam(EXIM_TRUST_GROUP);
    if (grp == NULL) {
        if (errno == 0)
            fprintf(stderr, "%s: No such group\n", EXIM_TRUST_GROUP);
        else
            perror(EXIM_TRUST_GROUP);
        exit(EXIT_FAILURE);
    }
    child_pid = fork();
    if (child_pid == 0) {
        close(STDIN_FILENO);
        if (dup2(pipefd[0], STDIN_FILENO) == -1) {
            perror(appname);
            exit(EXIT_FAILURE);
        }
        close(pipefd[0]);
        close(pipefd[1]);
        setegid(grp->gr_gid);
#ifndef DEBUG
        if (execl(EXIM_PATH, EXIM_PATH,
            "-bm",
            "-f", "<>",
            "-oi",
            "-oMt", "mailer-daemon",
            "-oMs virasto.finncora.net",
            "-t",
#else
        if (execl("/bin/cat", "/bin/cat",
#endif
            NULL
        ) == -1) {
            perror(EXIM_PATH);
            exit(EXIT_FAILURE);
        };
    }
    exim_out = fdopen(pipefd[1], "w");
    if (exim_out == NULL) {
        perror(appname);
        kill(child_pid, SIGKILL);
        exit(EXIT_FAILURE);
    }
}

void sanity_checks() {
    char *ret_path = get_header("Return-path");
    if (get_header("Envelope-to") == NULL || ret_path == NULL) {
        exit(EXIT_SUCCESS);
    }
    if (!strcmp(ret_path, "<>") || *ret_path == 0) {
        exit(EXIT_SUCCESS);
    }
    if (get_header("Received") == NULL) {
        fprintf(stderr, "%s: sanity check failed, no Received headers\n", appname);
        exit(EXIT_FAILURE);
    }
}

void start_output() {
    exec_exim();
    time_t t = time(NULL);
    srand(t);
    fprintf(exim_out, "Return-path: <>\n");
    fprintf(exim_out, "X-Failed-Recipients: %s\n", get_header("Envelope-to"));
    fprintf(exim_out, "Auto-Submitted: auto-replied\n");
    fprintf(exim_out, "From: %s\n", MAIL_FROM);
    fprintf(exim_out, "To: %s\n", get_header("Return-path"));
    fprintf(exim_out, "Subject: Mail delivery failed: returning message to sender\n");
    fprintf(exim_out, "Message-Id: <%04x-%04x-%04x@%s>\n",
            rand() & 0xffff, rand() & 0xffff, rand() & 0xffff, MSGID_DOMAIN);
    fprintf(exim_out, "Date: %s\n", ctime(&t));
    fprintf(exim_out, "\n");
    fprintf(exim_out, "This message was created automatically by mail delivery software.\n");
    fprintf(exim_out, "\n");
    fprintf(exim_out, "A message that you sent could not be delivered to one or more of its\n");
    fprintf(exim_out, "recipients. This is a permanent error. The following address(es) failed:\n");
    fprintf(exim_out, "\n");
    fprintf(exim_out, "  %s\n", get_header("Envelope-to"));
    fprintf(exim_out, "    No such user here\n");
    fprintf(exim_out, "\n");
    fprintf(exim_out, "------ This is a copy of the message, including all the headers. ------\n");
    fprintf(exim_out, "\n");
}

void dump_orig_headers() {
    for (struct header *hdr = all_headers; hdr; hdr = hdr->next) {
        fprintf(exim_out, "%s", hdr->line);
    }
    fprintf(exim_out, "\n");
}

void copy_msg() {
    char buf[READ_BUF_SIZE];
    size_t bytesread;
    while ((bytesread = fread(buf, 1, sizeof buf, stdin)) > 0) {
        size_t written = fwrite(buf, 1, bytesread, exim_out);
        if (written != bytesread) {
            fprintf(stderr, "%s: error piping message body to exim process. Errno=%d\n", appname, errno);
            kill(child_pid, SIGKILL);
            exit(EXIT_FAILURE);
        }
    }
}

void close_and_exit() {
    int status;
    fclose(exim_out);
    if (waitpid(child_pid, &status, 0) == -1) {
        perror("wait");
    } else {
        if (WIFSIGNALED(status)) {
            fprintf(stderr, "%s: exim crashed with signal %d\n", appname, WTERMSIG(status));
        } else if (WIFEXITED(status)) {
            exit(WEXITSTATUS(status));
        } else {
            fprintf(stderr, "%s: exim exited abnormally\n", appname);
        }
    }
    exit(EXIT_SUCCESS);
}

int main(int argc, char *argv[]) {
    appname = argv[0];
    read_headers();
    sanity_checks();
    start_output();
    dump_orig_headers();
    copy_msg();
    close_and_exit();
}
