#include <ncurses.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

// Constants for layout
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

// Config struct for .dmrc data
typedef struct {
    char username[256];
    char pwd[256];
    char cmd[512];
} Config;

// Global for signal handling
static volatile int running = 1;
static volatile int resized = 0;

// Signal handlers
void handle_sigint(int sig) {
    endwin(); // Restore terminal state
    exit(0);  // Exit immediately
}

void handle_sigwinch(int sig) {
    resized = 1;
}

// Free users array
void free_users(Config **users, int *num_users) {
    if (*users) {
        free(*users);
        *users = NULL;
    }
    *num_users = 0;
}

// Read ~/.dmrc file, supporting multiple users
int read_dmrc(Config **users, int *num_users) {
    *users = NULL;
    *num_users = 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/.dmrc", getenv("HOME"));
    FILE *file = fopen(path, "r");
    if (!file) return -1;

    char line[512];
    Config *temp_users = NULL;
    int capacity = 0;
    int current_user = -1;

    while (fgets(line, sizeof(line), file)) {
        char *start = line;
        while (*start == ' ' || *start == '\t') start++;
        if (*start == '\n' || *start == '#') continue;

        if (start[0] == '[' && start[strlen(start)-2] == ']') {
            current_user++;
            if (current_user >= capacity) {
                capacity = capacity ? capacity * 2 : 4;
                temp_users = realloc(temp_users, capacity * sizeof(Config));
                if (!temp_users) {
                    fclose(file);
                    return -1;
                }
            }
            memset(&temp_users[current_user], 0, sizeof(Config));
            (*num_users)++;
            continue;
        }

        char *eq = strchr(start, '=');
        if (!eq || current_user < 0) continue;
        *eq = '\0';
        char *key = start;
        char *value = eq + 1;

        while (key[strlen(key)-1] == ' ' || key[strlen(key)-1] == '\t')
            key[strlen(key)-1] = '\0';
        while (*value == ' ' || *value == '\t') value++;
        value[strcspn(value, "\n")] = '\0';

        if (!strcmp(key, "username"))
            strncpy(temp_users[current_user].username, value, sizeof(temp_users[current_user].username)-1);
        else if (!strcmp(key, "pwd"))
            strncpy(temp_users[current_user].pwd, value, sizeof(temp_users[current_user].pwd)-1);
        else if (!strcmp(key, "cmd"))
            strncpy(temp_users[current_user].cmd, value, sizeof(temp_users[current_user].cmd)-1);
    }
    fclose(file);

    *users = temp_users;
    return *num_users > 0 ? 0 : -1;
}

// Center text in a window
void print_centered(WINDOW *win, int row, int width, const char *text, int color_pair) {
    int len = strlen(text);
    int col = (width - len) / 2;
    wattron(win, color_pair);
    mvwprintw(win, row, col, "%s", text);
    wattroff(win, color_pair);
}

// Draw the UI, supporting selection and password modes
void draw_ui(WINDOW *win, int rows, int cols, Config *users, int num_users,
             int selected_user, int mode, const char *error_msg, int input_pos) {
    werase(win);
    box(win, 0, 0);

    print_centered(win, rows/2 + TITLE_ROW_OFFSET, cols, "Display Manager", COLOR_PAIR(1));
    print_centered(win, rows/2 + SEPARATOR_ROW_OFFSET, cols, "---------------------", 0);

    if (mode == SELECTION_MODE) {
        int start_row = rows/2 + USER_ROW_OFFSET;
        for (int i = 0; i < num_users; i++) {
            char user_line[512];
            snprintf(user_line, sizeof(user_line), "%s", users[i].username);
            if (i == selected_user) {
                wattron(win, A_REVERSE);
                print_centered(win, start_row + i, cols, user_line, COLOR_PAIR(1));
                wattroff(win, A_REVERSE);
            } else {
                print_centered(win, start_row + i, cols, user_line, COLOR_PAIR(1));
            }
        }
        print_centered(win, rows/2 + ERROR_ROW_OFFSET, cols, "Use arrows to select, Enter to confirm", 0);
    } else {
        char user_line[512];
        snprintf(user_line, sizeof(user_line), "User: %s", users[selected_user].username);
        print_centered(win, rows/2 + USER_ROW_OFFSET, cols, user_line, COLOR_PAIR(1));

        int prompt_col = cols * PROMPT_COL_FACTOR;
        mvwprintw(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col, "password: ");

        wattron(win, A_REVERSE);
        for (int i = 0; i < input_pos; i++) {
            mvwaddch(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + strlen("password: ") + i, '*');
        }
        wattroff(win, A_REVERSE);

        print_centered(win, rows/2 + KEYBINDS_ROW_OFFSET, cols, "Ctrl+C: Close, Esc 2x: Change User", 0);

        wmove(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + strlen("password: ") + input_pos);
    }

    if (error_msg[0]) {
        print_centered(win, rows/2 + ERROR_ROW_OFFSET, cols, error_msg, COLOR_PAIR(2));
    }

    wrefresh(win);
}

// Handle input, with proper error message timing
int handle_input(WINDOW *win, int rows, int cols, Config *users, int num_users,
                 int *selected_user, int *mode, char *input, int *pos, char *error_msg,
                 time_t *error_start) {
    static int esc_count = 0;
    static time_t last_esc_time = 0;
    int prompt_col = cols * PROMPT_COL_FACTOR + strlen("password: ");
    int ch = getch();

    if (*mode == SELECTION_MODE) {
        if (ch == KEY_UP && *selected_user > 0) {
            (*selected_user)--;
            draw_ui(win, rows, cols, users, num_users, *selected_user, *mode, error_msg, *pos);
            return 0;
        } else if (ch == KEY_DOWN && *selected_user < num_users - 1) {
            (*selected_user)++;
            draw_ui(win, rows, cols, users, num_users, *selected_user, *mode, error_msg, *pos);
            return 0;
        } else if (ch == '\n') {
            *mode = PASSWORD_MODE;
            input[0] = '\0';
            *pos = 0;
            esc_count = 0;
            draw_ui(win, rows, cols, users, num_users, *selected_user, *mode, error_msg, *pos);
            return 0;
        } else if (ch == 27) {
            *mode = PASSWORD_MODE;
            input[0] = '\0';
            *pos = 0;
            esc_count = 0;
            draw_ui(win, rows, cols, users, num_users, *selected_user, *mode, error_msg, *pos);
            return 0;
        }
    } else {
        if (ch == 27) {
            time_t current_time = time(NULL);
            if (current_time - last_esc_time <= 1) {
                esc_count++;
            } else {
                esc_count = 1;
            }
            last_esc_time = current_time;

            if (esc_count >= 2) {
                *mode = SELECTION_MODE;
                input[0] = '\0';
                *pos = 0;
                error_msg[0] = '\0';
                *error_start = 0;
                esc_count = 0;
                draw_ui(win, rows, cols, users, num_users, *selected_user, *mode, error_msg, *pos);
            }
            return 0;
        } else if (ch == '\n') {
            if (!strcmp(input, users[*selected_user].pwd)) {
                return 1;
            }
            strcpy(error_msg, "Incorrect password");
            input[0] = '\0';
            *pos = 0;
            *error_start = time(NULL);
            esc_count = 0;
            draw_ui(win, rows, cols, users, num_users, *selected_user, *mode, error_msg, *pos);
        } else if ((ch == KEY_BACKSPACE || ch == 127 || ch == 8) && *pos > 0) {
            input[--(*pos)] = '\0';
            mvwaddch(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + *pos, ' ');
            wmove(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + *pos);
            wrefresh(win);
            esc_count = 0;
        } else if (ch >= ' ' && ch <= '~' && *pos < MAX_PASSWORD_LEN) {
            input[(*pos)++] = ch;
            input[*pos] = '\0';
            wattron(win, A_REVERSE);
            mvwaddch(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + *pos - 1, '*');
            wattroff(win, A_REVERSE);
            wmove(win, rows/2 + PROMPT_ROW_OFFSET, prompt_col + *pos);
            wrefresh(win);
            esc_count = 0;
        } else {
            return 0;
        }
    }

    return 0;
}

int main() {
    Config *users = NULL;
    int num_users = 0;

    initscr();
    clear();
    refresh();
    if (!has_colors()) {
        endwin();
        fprintf(stderr, "Terminal does not support colors\n");
        return 1;
    }
    start_color();
    init_pair(1, COLOR_GREEN, COLOR_BLACK);
    init_pair(2, COLOR_RED, COLOR_BLACK);
    init_pair(3, COLOR_BLUE, COLOR_BLACK);
    noecho();
    cbreak();
    keypad(stdscr, TRUE);
    curs_set(1);

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    if (rows < MIN_ROWS || cols < MIN_COLS) {
        endwin();
        fprintf(stderr, "Terminal too small (min %dx%d)\n", MIN_COLS, MIN_ROWS);
        return 1;
    }

    int win_rows = rows * 0.8;
    int win_cols = cols * 0.8;
    if (win_rows > rows - 2) win_rows = rows - 2;
    if (win_cols > cols - 2) win_cols = cols - 2;
    int win_y = (rows - win_rows) / 2;
    int win_x = (cols - win_cols) / 2;
    WINDOW *win = newwin(win_rows, win_cols, win_y, win_x);
    wbkgd(win, COLOR_PAIR(3));

    signal(SIGINT, handle_sigint);
    signal(SIGWINCH, handle_sigwinch);

    char error_msg[256] = "";
    time_t error_start = 0;
    if (read_dmrc(&users, &num_users) != 0) {
        num_users = 1;
        users = malloc(sizeof(Config));
        strcpy(users[0].username, "guest");
        strcpy(users[0].pwd, "");
        strcpy(users[0].cmd, "true");
        strcpy(error_msg, "No ~/.dmrc found, using defaults");
        error_start = time(NULL);
        draw_ui(win, win_rows, win_cols, users, num_users, 0, PASSWORD_MODE, error_msg, 0);
        sleep(3);
        error_msg[0] = '\0';
        error_start = 0;
    }

    for (int i = 0; i < num_users; i++) {
        if (!users[i].username[0] || !users[i].cmd[0]) {
            strcpy(error_msg, "Invalid .dmrc: missing username or cmd");
            error_start = time(NULL);
            draw_ui(win, win_rows, win_cols, users, num_users, 0, PASSWORD_MODE, error_msg, 0);
            sleep(3);
            free_users(&users, &num_users);
            num_users = 1;
            users = malloc(sizeof(Config));
            strcpy(users[0].username, "guest");
            strcpy(users[0].pwd, "");
            strcpy(users[0].cmd, "true");
            error_msg[0] = '\0';
            error_start = 0;
            break;
        }
    }

    char input[MAX_PASSWORD_LEN + 1] = "";
    int pos = 0;
    int selected_user = 0;
    int mode = PASSWORD_MODE;

    draw_ui(win, win_rows, win_cols, users, num_users, selected_user, mode, error_msg, pos);
    usleep(100000);

    while (running) {
        if (resized) {
            resized = 0;
            clear();
            refresh();
            getmaxyx(stdscr, rows, cols);
            if (rows < MIN_ROWS || cols < MIN_COLS) {
                free_users(&users, &num_users);
                endwin();
                fprintf(stderr, "Terminal too small (min %dx%d)\n", MIN_COLS, MIN_ROWS);
                return 1;
            }
            win_rows = rows * 0.8;
            win_cols = cols * 0.8;
            if (win_rows > rows - 2) win_rows = rows - 2;
            if (win_cols > cols - 2) win_cols = cols - 2;
            win_y = (rows - win_rows) / 2;
            win_x = (cols - win_cols) / 2;
            delwin(win);
            win = newwin(win_rows, win_cols, win_y, win_x);
            wbkgd(win, COLOR_PAIR(3));
            draw_ui(win, win_rows, win_cols, users, num_users, selected_user, mode, error_msg, pos);
        }

        if (error_msg[0] && time(NULL) - error_start >= ERROR_TIMEOUT) {
            error_msg[0] = '\0';
            error_start = 0;
            draw_ui(win, win_rows, win_cols, users, num_users, selected_user, mode, error_msg, pos);
        }

        int action = handle_input(win, win_rows, win_cols, users, num_users,
                                 &selected_user, &mode, input, &pos, error_msg, &error_start);
        if (action == 1) {
            wclear(win);
            wrefresh(win);
            delwin(win);
            endwin();
            int ret = system(users[selected_user].cmd);
            free_users(&users, &num_users);
            if (ret != 0) {
                fprintf(stderr, "Command '%s' failed with code %d\n", users[selected_user].cmd, ret);
                initscr();
                clear();
                refresh();
                noecho();
                cbreak();
                keypad(stdscr, TRUE);
                curs_set(1);
                getmaxyx(stdscr, rows, cols);
                win_rows = rows * 0.8;
                win_cols = cols * 0.8;
                if (win_rows > rows - 2) win_rows = rows - 2;
                if (win_cols > cols - 2) win_cols = cols - 2;
                win_y = (rows - win_rows) / 2;
                win_x = (cols - win_cols) / 2;
                win = newwin(win_rows, win_cols, win_y, win_x);
                wbkgd(win, COLOR_PAIR(3));
                read_dmrc(&users, &num_users);
                if (num_users == 0) {
                    num_users = 1;
                    users = malloc(sizeof(Config));
                    strcpy(users[0].username, "guest");
                    strcpy(users[0].pwd, "");
                    strcpy(users[0].cmd, "true");
                }
                strcpy(error_msg, "Command failed, retrying login");
                error_start = time(NULL);
                input[0] = '\0';
                pos = 0;
                selected_user = 0;
                mode = PASSWORD_MODE;
                draw_ui(win, win_rows, win_cols, users, num_users, selected_user, mode, error_msg, pos);
                continue;
            }
            return 0;
        }
    }

    wclear(win);
    wrefresh(win);
    delwin(win);
    endwin();
    free_users(&users, &num_users);
    return 0;
}
