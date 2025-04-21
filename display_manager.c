#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

#define MIN_ROWS 10
#define MIN_COLS 40
#define TITLE_ROW_OFFSET -4
#define SEPARATOR_ROW_OFFSET -3
#define USER_ROW_OFFSET -2
#define PROMPT_ROW_OFFSET 0
#define ERROR_ROW_OFFSET 5
#define KEYBINDS_ROW_OFFSET 7
#define PROMPT_COL_FACTOR 0.25
#define MAX_PASSWORD_LEN 30
#define ERROR_TIMEOUT 2
#define SELECTION_MODE 0
#define PASSWORD_MODE 1

typedef struct {
    char username[256];
    char pwd[256];
    char cmd[512];
} Config;

Config *u=NULL;
int n=0;

static volatile int run=1;
static volatile int resz=0;

void free_users(Config **u, int *n);

void handle_sigint(int sig) {
  free_users(&u, &n);
  endwin();
  exit(0);
}

void handle_sigwinch(int sig) {
    resz=1;
}

void free_users(Config **u, int *n) {
  if (*u) {
      free(*u);
      *u = NULL;
  }
  *n=0;
}

int read_dmrc(Config **u,int *n) {
    *u=NULL;
    *n=0;
    char path[512];
    snprintf(path, sizeof(path), "%s/.dmrc", getenv("HOME"));
    FILE *file=fopen(path,"r");
    if (!file) return -1;

    char line[512];
    Config *temp=NULL;
    int cap=0;
    int cur=-1;

    while (fgets(line, sizeof(line), file)) {
        char *start=line;
        while (*start==' '||*start=='\t') start++;
        if (*start=='\n'||*start=='#') continue;

        if (start[0]=='['&&start[strlen(start)-2]==']') {
            cur++;
            if (cur>=cap) {
                cap=cap?cap*2:4;
                temp=realloc(temp,cap*sizeof(Config));
                if (!temp) {
                    fclose(file);
                    return -1;
                }
            }
            memset(&temp[cur],0,sizeof(Config));
            (*n)++;
            continue;
        }

        char *eq=strchr(start,'=');
        if (!eq||cur<0) continue;
        *eq='\0';
        char *key=start;
        char *val=eq+1;

        while (key[strlen(key)-1]==' '||key[strlen(key)-1]=='\t')
            key[strlen(key)-1]='\0';
        while (*val==' '||*val=='\t') val++;
        val[strcspn(val,"\n")]='\0';

        if (!strcmp(key,"username"))
            strncpy(temp[cur].username, val, sizeof(temp[cur].username)-1);
        else if (!strcmp(key, "pwd"))
            strncpy(temp[cur].pwd, val, sizeof(temp[cur].pwd)-1);
        else if (!strcmp(key, "cmd"))
            strncpy(temp[cur].cmd, val, sizeof(temp[cur].cmd)-1);
    }
    fclose(file);

    *u=temp;
    return *n>0?0:-1;
}

void print_centered(WINDOW *win,int row,int width, const char *text,int color) {
    int len=strlen(text);
    int col=(width-len)/2;
    wattron(win, color);
    mvwprintw(win,row,col,"%s",text);
    wattroff(win, color);
}

void draw_ui(WINDOW *win,int rows,int cols,Config *u,int n,int sel,int m, const char *err,int p) {
    werase(win);
    box(win,0,0);

    print_centered(win,rows/2+TITLE_ROW_OFFSET,cols,"Display Manager",COLOR_PAIR(1));
    print_centered(win, rows/2+SEPARATOR_ROW_OFFSET, cols, "---------------------", 0);

    if (m==SELECTION_MODE) {
      int start_row=rows/2+USER_ROW_OFFSET;
      for (int i=0;i<n;i++) {
          char user_line[512];
          snprintf(user_line,sizeof(user_line),"%s",u[i].username);
          if (i==sel) {
              wattron(win,A_REVERSE);
              print_centered(win,start_row+i,cols,user_line,COLOR_PAIR(1));
              wattroff(win,A_REVERSE);
          } else {
              print_centered(win,start_row+i,cols,user_line,COLOR_PAIR(1));
          }
      }
      print_centered(win,rows/2+ERROR_ROW_OFFSET,cols,"Use arrows to select, Enter to confirm",0);
    } else {
        char user_line[512];
        snprintf(user_line, sizeof(user_line), "User: %s", u[sel].username);
        print_centered(win, rows/2 + USER_ROW_OFFSET, cols, user_line, COLOR_PAIR(1));

        int prompt_col=cols*PROMPT_COL_FACTOR;
        mvwprintw(win,rows/2+PROMPT_ROW_OFFSET,prompt_col,"password: ");

        wattron(win,A_REVERSE);
        for (int i=0;i<p;i++) {
            mvwaddch(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + strlen("password: ") + i, '*');
        }
        wattroff(win, A_REVERSE);

        print_centered(win, rows/2 + KEYBINDS_ROW_OFFSET, cols, "Ctrl+C: Close, Esc 2x: Change User", 0);

        wmove(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + strlen("password: ") + p);
    }

    if (err[0]) {
        print_centered(win, rows/2 + ERROR_ROW_OFFSET, cols, err, COLOR_PAIR(2));
    }

    wrefresh(win);
}

int handle_input(WINDOW *win,int rows,int cols,Config *u,int n,int *sel,int *m,char *in,int *p,char *err,time_t *es) {
    static int esc_count=0;
    static time_t last_esc=0;
    int prompt_col=cols*PROMPT_COL_FACTOR+strlen("password: ");
    int ch=getch();

    if (*m==SELECTION_MODE) {
        if (ch==KEY_UP&&*sel>0) {
            (*sel)--;
            draw_ui(win,rows,cols,u,n,*sel,*m,err,*p);
            return 0;
        } else if (ch==KEY_DOWN&&*sel<n-1) {
            (*sel)++;
            draw_ui(win,rows,cols,u,n,*sel,*m,err,*p);
            return 0;
        } else if (ch=='\n') {
            *m=PASSWORD_MODE;
            in[0]='\0';
            *p=0;
            esc_count=0;
            draw_ui(win,rows,cols,u,n,*sel,*m,err,*p);
            return 0;
        } else if (ch==27) {
            *m=PASSWORD_MODE;
            in[0]='\0';
            *p=0;
            esc_count=0;
            draw_ui(win,rows,cols,u,n,*sel,*m,err,*p);
            return 0;
        }
    } else {
        if (ch==27) {
            time_t now=time(NULL);
            if (now-last_esc<=1) {
                esc_count++;
            } else {
                esc_count=1;
            }
            last_esc=now;

            if (esc_count>=2) {
                *m=SELECTION_MODE;
                in[0]='\0';
                *p=0;
                err[0]='\0';
                *es=0;
                esc_count=0;
                draw_ui(win,rows,cols,u,n,*sel,*m,err,*p);
            }
            return 0;
        } else if (ch=='\n') {
            if (!strcmp(in,u[*sel].pwd)) {
                return 1;
            }
            strcpy(err,"Incorrect password");
            in[0]='\0';
            *p=0;
            *es=time(NULL);
            esc_count=0;
            draw_ui(win,rows,cols,u,n,*sel,*m,err,*p);
        } else if ((ch==KEY_BACKSPACE||ch==127||ch==8)&&*p>0) {
            in[--(*p)]='\0';
            mvwaddch(win,rows/2+PROMPT_ROW_OFFSET,prompt_col+*p,' ');
            wmove(win,rows/2+PROMPT_ROW_OFFSET,prompt_col+*p);
            wrefresh(win);
            esc_count=0;
        } else if (ch>=' '&&ch<='~'&&*p<MAX_PASSWORD_LEN) {
            in[(*p)++]=ch;
            in[*p]='\0';
            wattron(win,A_REVERSE);
            mvwaddch(win,rows/2+PROMPT_ROW_OFFSET,prompt_col+*p-1,'*');
            wattroff(win,A_REVERSE);
            wmove(win,rows/2+PROMPT_ROW_OFFSET,prompt_col+*p);
            wrefresh(win);
            esc_count=0;
        } else {
            return 0;
        }
    }

    return 0;
}

int main() {
    initscr();
    clear();
    refresh();
    if (!has_colors()) {
        endwin();
        printf("Terminal does not support colors\n");
        return 1;
    }
    start_color();
    init_pair(1,COLOR_GREEN,COLOR_BLACK);
    init_pair(2,COLOR_RED,COLOR_BLACK);
    init_pair(3,COLOR_BLUE,COLOR_BLACK);
    noecho();
    cbreak();
    keypad(stdscr,TRUE);
    curs_set(1);

    int rows,cols;
    getmaxyx(stdscr,rows,cols);
    if (rows<MIN_ROWS||cols<MIN_COLS) {
        endwin();
        printf("Terminal too small (min %dx%d)\n",MIN_COLS,MIN_ROWS);
        return 1;
    }

    int win_rows=rows*0.8;
    int win_cols=cols*0.8;
    if (win_rows>rows-2) win_rows=rows-2;
    if (win_cols>cols-2) win_cols=cols-2;
    int win_y=(rows-win_rows)/2;
    int win_x=(cols-win_cols)/2;
    WINDOW *win=newwin(win_rows,win_cols,win_y,win_x);
    wbkgd(win,COLOR_PAIR(3));

    signal(SIGINT,handle_sigint);
    signal(SIGWINCH,handle_sigwinch);

    char err[256]="";
    time_t es=0;
    if (read_dmrc(&u,&n)!=0) {
        n=1;
        u=malloc(sizeof(Config));
        strcpy(u[0].username,"guest");
        strcpy(u[0].pwd,"");
        strcpy(u[0].cmd,"true");
        strcpy(err,"No ~/.dmrc found, using defaults");
        es=time(NULL);
        draw_ui(win,win_rows,win_cols,u,n,0,PASSWORD_MODE,err,0);
        sleep(3);
        err[0]='\0';
        es=0;
    }

    for (int i=0;i<n;i++) {
        if (!u[i].username[0]||!u[i].cmd[0]) {
            strcpy(err,"Invalid .dmrc: missing username or cmd");
            es=time(NULL);
            draw_ui(win,win_rows,win_cols,u,n,0,PASSWORD_MODE,err,0);
            sleep(3);
            free_users(&u,&n);
            n=1;
            u=malloc(sizeof(Config));
            strcpy(u[0].username,"guest");
            strcpy(u[0].pwd,"");
            strcpy(u[0].cmd,"true");
            err[0]='\0';
            es=0;
            break;
        }
    }

    char in[MAX_PASSWORD_LEN+1]="";
    int p=0;
    int sel=0;
    int m=PASSWORD_MODE;

    draw_ui(win,win_rows,win_cols,u,n,sel,m,err,p);
    usleep(100000);

    while (run) {
        if (resz) {
            resz=0;
            clear();
            refresh();
            getmaxyx(stdscr,rows,cols);
            if (rows<MIN_ROWS||cols<MIN_COLS) {
                free_users(&u,&n);
                endwin();
                printf("Terminal too small (min %dx%d)\n",MIN_COLS,MIN_ROWS);
                return 1;
            }
            win_rows=rows*0.8;
            win_cols=cols*0.8;
            if (win_rows>rows-2) win_rows=rows-2;
            if (win_cols>cols-2) win_cols=cols-2;
            win_y=(rows-win_rows)/2;
            win_x=(cols-win_cols)/2;
            delwin(win);
            win=newwin(win_rows,win_cols,win_y,win_x);
            wbkgd(win,COLOR_PAIR(3));
            draw_ui(win,win_rows,win_cols,u,n,sel,m,err,p);
        }

        if (err[0]&&time(NULL)-es>=ERROR_TIMEOUT) {
            err[0]='\0';
            es=0;
            draw_ui(win,win_rows,win_cols,u,n,sel,m,err,p);
        }

        int action=handle_input(win,win_rows,win_cols,u,n,&sel,&m,in,&p,err,&es);
        if (action==1) {
            wclear(win);
            wrefresh(win);
            delwin(win);
            endwin();
            int ret=system(u[sel].cmd);
            free_users(&u,&n);
            if (ret!=0) {
                printf("Command '%s' failed with code %d\n",u[sel].cmd,ret);
                initscr();
                clear();
                refresh();
                noecho();
                cbreak();
                keypad(stdscr,TRUE);
                curs_set(1);
                getmaxyx(stdscr,rows,cols);
                win_rows=rows*0.8;
                win_cols=cols*0.8;
                if (win_rows>rows-2) win_rows=rows-2;
                if (win_cols>cols-2) win_cols=cols-2;
                win_y=(rows-win_rows)/2;
                win_x=(cols-win_cols)/2;
                win=newwin(win_rows,win_cols,win_y,win_x);
                wbkgd(win,COLOR_PAIR(3));
                read_dmrc(&u,&n);
                if (n==0) {
                    n=1;
                    u=malloc(sizeof(Config));
                    strcpy(u[0].username,"guest");
                    strcpy(u[0].pwd,"");
                    strcpy(u[0].cmd,"true");
                }
                strcpy(err,"Command failed, retrying login");
                es=time(NULL);
                in[0]='\0';
                p=0;
                sel=0;
                m=PASSWORD_MODE;
                draw_ui(win,win_rows,win_cols,u,n,sel,m,err,p);
                continue;
            }
            return 0;
        }
    }

    wclear(win);
    wrefresh(win);
    delwin(win);
    endwin();
    free_users(&u,&n);
    return 0;
}
