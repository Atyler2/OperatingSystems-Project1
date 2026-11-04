#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>

#define MAX_COMMAND_LINE_LEN 1024
#define MAX_COMMAND_LINE_ARGS 128
#define MAX_PIPE_CMDS 16
#define TIMEOUT_SECONDS 10



char delimiters[] = " \t\r\n";
extern char **environ;

typedef struct {
  char *argv[MAX_COMMAND_LINE_ARGS];
  char *infile;
  char *outfile;
}Cmd;

static volatile pid_t fg_pids[MAX_PIPE_CMDS];
static volatile int fg_count = 0;
static volatile sig_atomic_t timed_out = 0;

static void sigint_handler(int sig){
  (void)sig;
  write(STDOUT_FILENO, "\n", 1);
}

static void sigalrm_handler(int sig){
  (void)sig;
  timed_out = 1;
  for (int i = 0; i < fg_count; i++){
    if (fg_pids[i]>0) kill(fg_pids[i],SIGKILL);
  }
}

static void print_prompt(void){
  char cwd[MAX_COMMAND_LINE_LEN];
  if(getcwd(cwd, sizeof(cwd))== NULL){
    perror("getcwd() error");
    strcpy(cwd, "?");
  }
  printf("%s> ", cwd);
  fflush(stdout);
}

static int tokenize(char *line, char *args[]){
  int n = 0; 
  char *tok = strtok(line, delimiters);
  while(tok != NULL && n < MAX_COMMAND_LINE_ARGS -1){
    if(tok[0] == '$' && tok[1] != '\0'){
      char *val = getenv(tok + 1);
      args[n++] = val ? val : "";
    }else {
      args[n++] = tok; 
    }
    tok = strtok(NULL, delimiters);
  }
  args[n] = NULL;
  return n;
}

static bool is_builtin(const char *name){
  return strcmp(name, "cd") == 0 || strcmp(name, "pwd") == 0||
         strcmp(name, "echo") == 0 || strcmp(name, "exit") == 0 ||
         strcmp(name, "env") == 0 || strcmp(name, "setenv") == 0;
}

static void run_builtin(char *argv[]){
  if(strcmp(argv[0], "cd") == 0){
    const char *dir = argv[1] ? argv[1] : getenv("HOME");
    if(dir == NULL || chdir(dir) != 0) perror("cd() failed");
  }else if (strcmp(argv[0], "pwd") == 0){
    char cwd[MAX_COMMAND_LINE_LEN];
    if (getcwd(cwd,sizeof(cwd)) !=NULL) printf("%s\n", cwd);
    else perror("getcwd() failed");
  }else if (strcmp (argv[0], "echo") == 0){
    for (int i = 1; argv[i] != NULL; i++){
      printf("%s%s", argv[i], argv[i + 1] ? " ": "");
    }
    printf("\n");
  }else if (strcmp(argv[0], "exit") == 0){
    exit(0);
  }else if (strcmp(argv[0], "env") == 0) {
    if (argv[1] == NULL){
      for(int i = 0; environ[i] != NULL; i++) printf("%s\n", environ[i]);
    }else {
      char *val = getenv(argv[1]);
      if (val) printf("%s\n", val);
    }
  }else if (strcmp(argv[0], "setenv")==0){
    if (argv[1] == NULL){
      fprintf(stderr, "setenv: usage: setenv NAME=VALUE or setenv NAME VALUE\n");
    }else if (argv[2] != NULL){
      if (setenv(argv[1], argv[2], 1) != 0) perror("setenv() failed");
    }else{
      char *eq = strchr(argv[1], '=');
      if(eq == NULL) {
        if (setenv(argv[1], "", 1) !=0) perror("setenv() failed");
      }else {
        *eq = '\0';
        if(setenv(argv[1], eq + 1, 1)!=0) perror("setenv() failed");
      }
    }
  }
  fflush(stdout);
} 

static int parse_commands(char *tokens[], int ntokens, Cmd cmds[]){
  int nc = 0;
  int argc = 0;
  memset(&cmds[0], 0, sizeof(Cmd));

  for(int i = 0; i < ntokens; i++){
    if (strcmp(tokens[i], "|") == 0){
      if(argc == 0 || nc + 1 >= MAX_PIPE_CMDS) return -1;
      cmds[nc].argv[argc] = NULL;
      nc++;
      argc = 0;
      memset(&cmds[nc], 0, sizeof(Cmd));
    }else if (strcmp(tokens[i], ">") == 0){
      if(i + 1 >= ntokens) return -1;
      cmds[nc].outfile = tokens[++i];
    }else if(strcmp(tokens[i], "<") == 0){
      if(i + 1 >= ntokens) return -1;
      cmds[nc].infile = tokens[++i];
    }else {
      cmds[nc].argv[argc++] = tokens[i];
    }

  }
  if(argc == 0) return -1;
  cmds[nc].argv[argc] = NULL;
  return nc + 1;
}

static int apply_redirects(const Cmd *cmd){
  if(cmd -> infile){
    int fd = open(cmd->infile, O_RDONLY);
    if(fd<0) {perror(cmd->infile); return -1;}
    dup2(fd, STDIN_FILENO);
    close(fd);
  }
  if (cmd->outfile){
    int fd = open(cmd->outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644 );
    if(fd < 0){perror(cmd->outfile); return -1;}
    dup2(fd, STDOUT_FILENO);
    close(fd);
  }
  return 0;
}

static void run_pipeline(Cmd cmds[], int n, bool bg){
  int prev_read = -1;
  pid_t pids[MAX_PIPE_CMDS];
  int npids = 0;

  for (int i = 0; i < n; i++){
    int pfd[2] = {-1, -1};
    if (i < n - 1 && pipe(pfd) < 0){
      perror("pipe() failed");
      break;
    }

    pid_t pid = fork();
    if(pid < 0){
      perror("fork() failed");
      if(pfd[0] >= 0) {close(pfd[0]); close(pfd[1]);}
      break;
    }
    
    if (pid == 0){
      signal(SIGINT, bg ? SIG_IGN : SIG_DFL);
      signal(SIGALRM, SIG_DFL);

      if(prev_read >= 0) {dup2(prev_read, STDIN_FILENO); close(prev_read);}
      if(pfd[1] >= 0) { dup2(pfd[1], STDOUT_FILENO); close(pfd[0]);  close(pfd[1]);}

      if (apply_redirects(&cmds[i])< 0) exit(1);

      if(is_builtin(cmds[i].argv[0])){
        run_builtin(cmds[i].argv);
        exit(0);
      }
      execvp(cmds[i].argv[0],cmds[i].argv);
      perror("execvp() failed");
      fprintf(stderr, "An error occurred.\n");
      exit(1);
    }

  

  pids[npids++] = pid;
  if (prev_read >= 0) close(prev_read);
  if (pfd[1]>= 0) close(pfd[1]);
  prev_read = pfd[0];
  }

if (prev_read >= 0) close(prev_read);

if (bg) return;

timed_out = 0;
fg_count = npids;
for (int i = 0; i < npids; i++) fg_pids[i] = pids[i];
alarm(TIMEOUT_SECONDS);

for (int i = 0; i < npids; i++){
  int status;
  while (waitpid(pids[i], &status, 0) < 0 && errno == EINTR){
  }
  fg_pids[i] = 0; 
}
alarm(0);
fg_count = 0;

if (timed_out){
  printf("Process terminated: exceeded %d second time limit\n", TIMEOUT_SECONDS);
  fflush(stdout);
}
}
int main() {
    // Stores the string typed into the command line.
    char command_line[MAX_COMMAND_LINE_LEN];
  
    // Stores the tokenized command line input.
    char *arguments[MAX_COMMAND_LINE_ARGS];
    Cmd cmds[MAX_PIPE_CMDS];


    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sa.sa_handler = sigint_handler;
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = sigalrm_handler;
    sigaction(SIGALRM, &sa, NULL);

    while (true) {

        while (waitpid(-1, NULL, WNOHANG) > 0 ){}

        bool got_line = false;
        while (!got_line){
          print_prompt();

          if (fgets(command_line, MAX_COMMAND_LINE_LEN, stdin) == NULL){
            if(errno == EINTR){
              clearerr(stdin);
              errno = 0;
              continue;
            }
            if (ferror(stdin)){
              fprintf(stderr, "fgets error");
              exit(0);
            }
            printf("\n");
            fflush(stdout);
            fflush(stderr);
            return 0;
          }
          if (command_line[0] != '\n') got_line = true;
        }

        command_line[strcspn(command_line, "\n")] = '\0';

        int ntokens = tokenize(command_line, arguments);
        if (ntokens == 0) continue;

        bool background = false;
        if (strcmp(arguments[ntokens - 1], "&")==0){
          background = true;
          arguments[--ntokens] = NULL;
          if (ntokens == 0) continue; 
        }

        int ncmds = parse_commands(arguments, ntokens ,cmds);
        if(ncmds < 0){
          fprintf(stderr, "Syntax error\n");
          continue;
        }
        assert(ncmds >= 1);

        if (ncmds == 1 && is_builtin(cmds[0].argv[0])){
          int saved_in = dup(STDIN_FILENO);
          int saved_out = dup(STDOUT_FILENO);
          if (apply_redirects(&cmds[0]) == 0) run_builtin(cmds[0].argv);
          fflush(stdout);
          dup2(saved_in, STDIN_FILENO);
          dup2(saved_out, STDOUT_FILENO);
          close(saved_in);
          close(saved_out);
          continue; 
        }
        run_pipleLine(cmds, ncmds, background);
      } 
      return -1;
    }
    