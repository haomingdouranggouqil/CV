/*** includes ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>                      //Linux/Unix系统调用库
#include <termios.h>                     //Linux控制台库

/*----------------------define--------------------------*/

#define VERSION "0.0.1"
#define CTRL_KEY(k) ((k) & 0x1f)
#define TAB_STOP 4                       //Tab占4空格
#define QUIT_TIMES 3                     //忽视警告退出时连按三次
#define BUF_INIT {NULL, 0}
#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*----------------------提前声明函数原型------------------------*/

void set_status_message(const char *fmt, ...);
void refresh_screen();
char *editor_prompt(char *prompt, void (*callback)(char *, int));

/*----------------------枚举类型与结构体定义----------------------*/

//功能键对应数值
enum function_key 
{
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    PAGE_UP,
    PAGE_DOWN
};

//不同类型对应不同高亮颜色
enum editor_highlight 
{
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};


//语法高亮结构体
struct editor_syntax 
{
    char *filetype;
    char **filematch;
    char **keywords;
    char *singleline_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

//存储行文本
typedef struct erow 
{
    int idx;
    int size;
    int rsize;
    char *chars;          //字符
    char *render;         //符号
    unsigned char *hl;    //高亮标志
    int hl_open_comment;  //注释高亮
} erow;

struct editor_config 
{
    int cx, cy;           //光标位置
    int rx;               //符号索引
    int rowoff;           //行偏移
    int coloff;           //列偏移
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;            //修改标志
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct editor_syntax *syntax;
    struct termios origin_termios;
};

//全局状态初始化
struct editor_config G;

/*----------------------终端设置-------------------------*/

//报错函数
void warn(const char *s) 
{
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

//退出时禁用原始模式
void disable_raw_mode() 
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &G.origin_termios) == -1)
    {
        warn("tcsetattr");
    }
}

//启用原始模式，关闭回声，关闭功能键
void enable_raw_mode() 
{
    if (tcgetattr(STDIN_FILENO, &G.origin_termios) == -1)
    {
        warn("tcgetattr");
    }
    atexit(disable_raw_mode);

    struct termios raw = G.origin_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
    {
        warn("tcsetattr");
    }
}

//得到光标位置
int cursor_position(int *rows, int *cols) 
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
    {
        return -1;
    }

    while (i < sizeof(buf) - 1) 
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
        {
            break;
        }
        if (buf[i] == 'R')
        {
            break;
        }
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
    {
        return -1;
    }
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
    {
        return -1;
    }

    return 0;
}

//得到窗口大小
int window_size(int *rows, int *cols) 
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) 
    {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
        {
            return -1;
        }
        return cursor_position(rows, cols);
    } 
    else 
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*---------------------------创建缓冲区-----------------------------*/


//创建缓冲结构体
struct buffer 
{
    char *b;
    int len;
};

//添加缓冲区
void buf_append(struct buffer *ab, const char *s, int len) 
{
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL)
    {
        return;
    }
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

//释放缓冲区
void buf_free(struct buffer *ab) 
{
    free(ab->b);
}
/*----------------------------输入----------------------------------*/

//等待一个按键并返回值，功能键特殊判断
int read_key() 
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1) 
    {
        if (nread == -1 && errno != EAGAIN)
        {
            warn("read");
        }
    }

    if (c == '\x1b') 
    {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1)
        {
            return '\x1b';
        }
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
        {
            return '\x1b';
        }

        if (seq[0] == '[') 
        {
            if (seq[1] >= '0' && seq[1] <= '9') 
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                {
                    return '\x1b';
                }
                if (seq[2] == '~')
                {
                    switch (seq[1]) 
                    {
                        case '3': 
                            return DEL_KEY;
                        case '5': 
                            return PAGE_UP;
                        case '6': 
                            return PAGE_DOWN;
                    }
                }
            } 
            else 
            {
                switch (seq[1]) 
                {
                    case 'A': 
                        return ARROW_UP;
                    case 'B': 
                        return ARROW_DOWN;
                    case 'C': 
                        return ARROW_RIGHT;
                    case 'D': 
                        return ARROW_LEFT;
                }
            }
        } 
        return '\x1b';
    } 
    else 
    {
        return c;
    }
}

//上下左右移动光标
void move_cursor(int key) 
{
    erow *row = (G.cy >= G.numrows) ? NULL : &G.row[G.cy];

    switch (key) 
    {
        case ARROW_LEFT:
            if (G.cx != 0) 
            {
                G.cx--;
            } 
            else if (G.cy > 0) 
            {
                G.cy--;
                G.cx = G.row[G.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && G.cx < row->size) 
            {
                G.cx++;
            } 
            else if (row && G.cx == row->size) 
            {
                G.cy++;
                G.cx = 0;
            }
            break;
        case ARROW_UP:
            if (G.cy != 0) 
            {
                G.cy--;
            }
            break;
        case ARROW_DOWN:
            if (G.cy < G.numrows) 
            {
                G.cy++;
            }
            break;
    }

    row = (G.cy >= G.numrows) ? NULL : &G.row[G.cy];
    int rowlen = row ? row->size : 0;
    if (G.cx > rowlen) 
    {
        G.cx = rowlen;
    }
}

//处理输入内容以及各种功能键，输入Ctrl-q退出程序
void process_key() 
{
    static int quit_times = QUIT_TIMES;
        //read_key()函数读取字节到c中，
    int c = read_key();

    switch (c) 
    {
        case '\r':
            insert_new_line();
            break;
        //输入Ctrl-q退出程序并清屏
        case CTRL_KEY('q'):
            //有未保存退出时提醒按键三次
            if (G.dirty && quit_times > 0) 
            {
                set_status_message("WARNING!!! File has unsaved changes. Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case CTRL_KEY('s'):
            save();
            break;
        case CTRL_KEY('p'):
        {
            //先保存未保存的变化
            save();
            //运行python代码
            char sh[80];
    	    strcpy (sh,"gnome-terminal -- bash -c \"python3 ");
    	    strcat (sh,G.filename);
    	    strcat (sh, "; exec bash\"");
            system(sh);
        }
            break;
        case CTRL_KEY('r'):
        {
            //先保存未保存的变化
            save();
            //运行c代码
            char sh[80];
    	    strcpy (sh,"gcc ");
    	    strcat (sh,G.filename);
            system(sh);
            system("gnome-terminal -- bash -c \"./a.out; exec bash\"");
        }
            break;

        case CTRL_KEY('f'):
            find();
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY)
            {
                move_cursor(ARROW_RIGHT);
            }
            editor_del_char();
            break;

        case PAGE_UP:
        case PAGE_DOWN:
        {
            if (c == PAGE_UP) 
            {
                G.cy = G.rowoff;
            } 
            else if (c == PAGE_DOWN) 
            {
                G.cy = G.rowoff + G.screenrows - 1;
                if (G.cy > G.numrows)
                {
                    G.cy = G.numrows;
                }
            }
            int times = G.screenrows;
            while (times--)
            {
                move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
        }
            break;

        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            move_cursor(c);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editor_insert_char(c);
            break;
    }
    quit_times = QUIT_TIMES;
}

/*--------------------输出---------------------*/

//翻卷
void scroll() 
{
    G.rx = 0;
    if (G.cy < G.numrows) 
    {
        G.rx = cx_to_rx(&G.row[G.cy], G.cx);
    }

    if (G.cy < G.rowoff) 
    {
        G.rowoff = G.cy;
    }
    if (G.cy >= G.rowoff + G.screenrows) 
    {
        G.rowoff = G.cy - G.screenrows + 1;
    }
    if (G.rx < G.coloff) 
    {
        G.coloff = G.rx;
    }
    if (G.rx >= G.coloff + G.screencols) 
    {
        G.coloff = G.rx - G.screencols + 1;
    }
}


//像vim一样画波浪线,语法高亮，并显示版本信息
void draw_rows(struct buffer *ab) 
{
    int y;
    for (y = 0; y < G.screenrows; y++) 
    {
        int filerow = y + G.rowoff;
        if (filerow >= G.numrows) 
        {
            //1/3处显示信息
            if (G.numrows == 0 && y == G.screenrows / 3) 
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),"CV editor -- version %s", VERSION);
                if (welcomelen > G.screencols)
                {
                    welcomelen = G.screencols;
                }
                //居中显示
                int padding = (G.screencols - welcomelen) / 2;
                if (padding) 
                {
                    buf_append(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                {
                    buf_append(ab, " ", 1);
                }
                buf_append(ab, welcome, welcomelen);
            } 
            else 
            {
                buf_append(ab, "~", 1);
            }
        } 
        else 
        {
            int len = G.row[filerow].rsize - G.coloff;
            if (len < 0)
            {
                len = 0;
            }
            if (len > G.screencols)
            {
                len = G.screencols;
            }
            char *c = &G.row[filerow].render[G.coloff];
            unsigned char *hl = &G.row[filerow].hl[G.coloff];
            int current_color = -1;
            int j;
            //加转义字符高亮
            for (j = 0; j < len; j++) 
            {
                if (iscntrl(c[j])) 
                {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    buf_append(ab, "\x1b[7m", 4);
                    buf_append(ab, &sym, 1);
                    buf_append(ab, "\x1b[m", 3);
                    if (current_color != -1) 
                    {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        buf_append(ab, buf, clen);
                    }
                } 
                else if (hl[j] == HL_NORMAL) 
                {
                    if (current_color != -1) 
                    {
                        buf_append(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    buf_append(ab, &c[j], 1);
                } 
                else 
                {
                    int color = syntax_color(hl[j]);
                    if (color != current_color) 
                    {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        buf_append(ab, buf, clen);
                    }
                    buf_append(ab, &c[j], 1);
                }
            }
            buf_append(ab, "\x1b[39m", 5);
        }
        buf_append(ab, "\x1b[K", 3);
        buf_append(ab, "\r\n", 2);
    }
}



//清屏，显示状态栏，并将光标移动到原先位置
void refresh_screen() 
{
    scroll();

    struct buffer ab = BUF_INIT;

    buf_append(&ab, "\x1b[?25l", 6);
    buf_append(&ab, "\x1b[H", 3);

    draw_rows(&ab);
    draw_status_bar(&ab);
    draw_message_bar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (G.cy - G.rowoff) + 1, (G.rx - G.coloff) + 1);
    buf_append(&ab, buf, strlen(buf));

    buf_append(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    buf_free(&ab);
}

//绘制状态栏
void draw_status_bar(struct buffer *ab) 
{
    buf_append(ab, "\x1b[7m", 4);
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        G.filename ? G.filename : "[No Name]", G.numrows,
        G.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
        G.syntax ? G.syntax->filetype : "no ft", G.cy + 1, G.numrows);
    if (len > G.screencols)
    {
        len = G.screencols;
    }
    buf_append(ab, status, len);
    while (len < G.screencols) 
    {
        if (G.screencols - len == rlen) 
        {
            buf_append(ab, rstatus, rlen);
            break;
        } 
        else 
        {
            buf_append(ab, " ", 1);
            len++;
        }
    }
    buf_append(ab, "\x1b[m", 3);
    buf_append(ab, "\r\n", 2);
}

//显示提示函数
char *editor_prompt(char *prompt, void (*callback)(char *, int)) 
{
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) 
    {
        set_status_message(prompt, buf);
        refresh_screen();

        int c = read_key();
        if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) 
        {
            if (buflen != 0)
            {
                buf[--buflen] = '\0';
            }
        } 
        else if (c == '\x1b') 
        {
            set_status_message("");
            if (callback)
            {
                callback(buf, c);
            }
            free(buf);
            return NULL;
        } 
        else if (c == '\r') 
        {
            if (buflen != 0) 
            {
                set_status_message("");
                if (callback)
                {
                    callback(buf, c);
                }
                return buf;
            }
        } 
        else if (!iscntrl(c) && c < 128) 
        {
            if (buflen == bufsize - 1) 
            {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback)
        {
            callback(buf, c);
        }
    }
}


//设置状态栏
void set_status_message(const char *fmt, ...) 
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(G.statusmsg, sizeof(G.statusmsg), fmt, ap);
    va_end(ap);
    G.statusmsg_time = time(NULL);
}

void draw_message_bar(struct buffer *ab) 
{
    buf_append(ab, "\x1b[K", 3);
    int msglen = strlen(G.statusmsg);
    if (msglen > G.screencols)
    {
        msglen = G.screencols;
    }
    if (msglen && time(NULL) - G.statusmsg_time < 5)
    {
        buf_append(ab, G.statusmsg, msglen);
    }
}



/*-----------------------文件操作--------------------------*/

//为了处理Tab这种一个符号占多个字符的情况，需建立字符索引和符号索引并相互转换
int cx_to_rx(erow *row, int cx) 
{
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) 
    {
        if (row->chars[j] == '\t')
        {
            rx += (TAB_STOP - 1) - (rx % TAB_STOP);
        }
        rx++;
    }
    return rx;
}

int rx_to_cx(erow *row, int rx) 
{
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) 
    {
        if (row->chars[cx] == '\t')
        {
            cur_rx += (TAB_STOP - 1) - (cur_rx % TAB_STOP);
        }
        cur_rx++;
        if (cur_rx > rx)
        {
            return cx;
        }
    }
    return cx;
}


//更新行
void update_row(erow *row) 
{
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
    {
        if (row->chars[j] == '\t')
        {
            tabs++;
        }
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) 
    {
        if (row->chars[j] == '\t') 
        {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP != 0)
            {
                row->render[idx++] = ' ';
            }
        } 
        else 
        {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;

    update_syntax(row);
}

//插入字符
void row_insert_char(erow *row, int at, int c) 
{
    if (at < 0 || at > row->size)
    {
        at = row->size;
    }
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    update_row(row);
    G.dirty++;
}


//Enter键插入新行
void insert_new_line() 
{
    if (G.cx == 0) 
    {
        editor_insert_row(G.cy, "", 0);
    } 
    else 
    {
        erow *row = &G.row[G.cy];
        editor_insert_row(G.cy + 1, &row->chars[G.cx], row->size - G.cx);
        row = &G.row[G.cy];
        row->size = G.cx;
        row->chars[row->size] = '\0';
        update_row(row);
    }
    G.cy++;
    G.cx = 0;
}

void row_append_string(erow *row, char *s, size_t len) 
{
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    update_row(row);
    G.dirty++;
}



//实现退格各种功能
void row_del_char(erow *row, int at) 
{
    if (at < 0 || at >= row->size)
    {
        return;
    }
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    update_row(row);
    G.dirty++;
}


//删除行
void free_row(erow *row) 
{
    free(row->render);
    free(row->chars);
    free(row->hl);
}

//行首退格删除行
void editor_del_row(int at) 
{
    if (at < 0 || at >= G.numrows)
    {
        return;
    }
    free_row(&G.row[at]);
    memmove(&G.row[at], &G.row[at + 1], sizeof(erow) * (G.numrows - at - 1));
    for (int j = at; j < G.numrows - 1; j++)
    {
        G.row[j].idx--;
    }
    G.numrows--;
    G.dirty++;
}

//行首退格将本行字符串附加到上一行行尾
void editor_insert_row(int at, char *s, size_t len) 
{
    if (at < 0 || at > G.numrows)
    {
        return;
    }

    G.row = realloc(G.row, sizeof(erow) * (G.numrows + 1));
    memmove(&G.row[at + 1], &G.row[at], sizeof(erow) * (G.numrows - at));
    for (int j = at + 1; j <= G.numrows; j++)
    {
        G.row[j].idx++;
    }

    G.row[at].idx = at;

    G.row[at].size = len;
    G.row[at].chars = malloc(len + 1);
    memcpy(G.row[at].chars, s, len);
    G.row[at].chars[len] = '\0';

    G.row[at].rsize = 0;
    G.row[at].render = NULL;
    G.row[at].hl = NULL;
    G.row[at].hl_open_comment = 0;
    update_row(&G.row[at]);

    G.numrows++;
    G.dirty++;
}

//打开文件
void editor_open(char *filename) 
{
    free(G.filename);
    G.filename = strdup(filename);

    select_highlight();

    FILE *fp = fopen(filename, "r");
    if (!fp)
    {
        warn("fopen");
    } 

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) 
    {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
        {
            linelen--;
        }
        editor_insert_row(G.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    G.dirty = 0;
}

//将行内容转化为字符串
char *rows_to_string(int *buflen) 
{
    int totlen = 0;
    int j;
    for (j = 0; j < G.numrows; j++)
    {
        totlen += G.row[j].size + 1;
    }
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (j = 0; j < G.numrows; j++) 
    {
        memcpy(p, G.row[j].chars, G.row[j].size);
        p += G.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

//保存内容到磁盘
void save() 
{
    if (G.filename == NULL) 
    {
        G.filename = editor_prompt("Save as: %s (ESC to cancel)", NULL);
        if (G.filename == NULL) 
        {
            set_status_message("Save aborted");
            return;
        }
        select_highlight();
    }

    int len;
    char *buf = rows_to_string(&len);

    int fd = open(G.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) 
    {
        if (ftruncate(fd, len) != -1) 
        {
            if (write(fd, buf, len) == len) 
            {
                close(fd);
                free(buf);
                G.dirty = 0;
                set_status_message("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }
    free(buf);
    set_status_message("Can't save! I/O error: %s", strerror(errno));
}

/*--------------------------编辑器操作------------------*/

//编辑器调用editorRowInsertChar函数实现添加字符功能
void editor_insert_char(int c) 
{
    if (G.cy == G.numrows) 
    {
        editor_insert_row(G.numrows, "", 0);
    }
    row_insert_char(&G.row[G.cy], G.cx, c);
    G.cx++;
}

//编辑器实现退格
void editor_del_char() 
{
    if (G.cy == G.numrows)
    {
        return;
    }
    if (G.cx == 0 && G.cy == 0)
    {
        return;
    }
    erow *row = &G.row[G.cy];
    if (G.cx > 0) 
    {
        row_del_char(row, G.cx - 1);
        G.cx--;
    } 
    else 
    {
        G.cx = G.row[G.cy - 1].size;
        row_append_string(&G.row[G.cy - 1], row->chars, row->size);
        editor_del_row(G.cy);
        G.cy--;
    }
}

/*-----------------------搜索---------------------------*/

//搜索匹配字符并高亮
void find_call_back(char *query, int key) 
{
    static int last_match = -1;
    static int direction = 1;

    static int saved_hl_line;
    static char *saved_hl = NULL;

    if (saved_hl) 
    {
        memcpy(G.row[saved_hl_line].hl, saved_hl, G.row[saved_hl_line].rsize);
        free(saved_hl);
        saved_hl = NULL;
    }

    if (key == '\r' || key == '\x1b') 
    {
        last_match = -1;
        direction = 1;
        return;
    } 
    else if (key == ARROW_RIGHT || key == ARROW_DOWN)
    {
        direction = 1;
    } 
    else if (key == ARROW_LEFT || key == ARROW_UP) 
    {
        direction = -1;
    } 
    else 
    {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int current = last_match;
    int i;
    for (i = 0; i < G.numrows; i++) 
    {
        current += direction;
        if (current == -1)
        {
            current = G.numrows - 1;
        }
        else if (current == G.numrows)
        {
            current = 0;
        }
        erow *row = &G.row[current];
        char *match = strstr(row->render, query);
        if (match) 
        {
            last_match = current;
            G.cy = current;
            G.cx = rx_to_cx(row, match - row->render);
            G.rowoff = G.numrows;

            saved_hl_line = current;
            saved_hl = malloc(row->rsize);
            memcpy(saved_hl, row->hl, row->rsize);
            memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}



void find() 
{
    //取消搜索时回复光标位置
    int saved_cx = G.cx;
    int saved_cy = G.cy;
    int saved_coloff = G.coloff;
    int saved_rowoff = G.rowoff;

    char *query = editor_prompt("Search: %s (Use ESC/Arrows/Enter)",
                                find_call_back);

    if (query) 
    {
        free(query);
    } 
    else 
    {
        G.cx = saved_cx;
        G.cy = saved_cy;
        G.coloff = saved_coloff;
        G.rowoff = saved_rowoff;
    }
}

/*-----------------------语法高亮----------------------*/

char *C_HL_extensions[] = { ".c", ".h", ".cpp", NULL };
char *Py_HL_extensions[] = {".py", NULL };

//语法高亮关键词

char *C_HL_keywords[] = 
{
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "or", "case",

    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|",  NULL
};

char *Py_HL_keywords[] = 
{
    "if", "while", "for", "break", "continue", "return", "else", "or", 
    "as", "assert", "not", "del", "elif", "except", "False", "finally",
    "from", "global", "import", "in", "is", "lambda", "None", "nonlocal",
    "pass", "raise", "try", "with", "yield", "True","and",


    "def|", "class|", NULL
};

struct editor_syntax HLDB[] = 
{
    {
        "c",
        C_HL_extensions,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
    {
        "py",
        Py_HL_extensions,
        Py_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};



//判断是否是分隔符
int is_separator(int c) 
{
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}



void update_syntax(erow *row) 
{
    row->hl = realloc(row->hl, row->rsize);
    memset(row->hl, HL_NORMAL, row->rsize);

    if (G.syntax == NULL)
    {
        return;
    }

    char **keywords = G.syntax->keywords;

    char *scs = G.syntax->singleline_comment_start;
    char *mcs = G.syntax->multiline_comment_start;
    char *mce = G.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_sep = 1;
    int in_string = 0;
    int in_comment = (row->idx > 0 && G.row[row->idx - 1].hl_open_comment);

    int i = 0;
    while (i < row->rsize) 
    {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment) 
        {
            if (!strncmp(&row->render[i], scs, scs_len)) 
            {
                memset(&row->hl[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        if (mcs_len && mce_len && !in_string) 
        {
            if (in_comment) 
            {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) 
                {
                    memset(&row->hl[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                    continue;
                } 
                else 
                {
                    i++;
                    continue;
                }
            } 
            else if (!strncmp(&row->render[i], mcs, mcs_len)) 
            {
                memset(&row->hl[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (G.syntax->flags & HL_HIGHLIGHT_STRINGS) 
        {
            if (in_string) 
            {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) 
                {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string)
                {
                    in_string = 0;
                }
                i++;
                prev_sep = 1;
                continue;
            } 
            else 
            {
                if (c == '"' || c == '\'') 
                {
                    in_string = c;
                    row->hl[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if (G.syntax->flags & HL_HIGHLIGHT_NUMBERS) 
        {
            if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) 
            {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep) 
        {
            int j;
            for (j = 0; keywords[j]; j++) 
            {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2)
                {
                    klen--;
                }

                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen])) 
                {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) 
            {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->idx + 1 < G.numrows)
    {
        update_syntax(&G.row[row->idx + 1]);
    }
}


//根据不同关键词类型返回不同颜色
int syntax_color(int hl) 
{
    switch (hl) 
    {
        case HL_COMMENT:
        case HL_MLCOMMENT: 
            return 36;
        case HL_KEYWORD1: 
            return 33;
        case HL_KEYWORD2: 
            return 32;
        case HL_STRING: 
            return 35;
        case HL_NUMBER: 
            return 31;
        case HL_MATCH: 
            return 34;
        default: 
            return 37;
    }
}


//根据文件类型判断是否该语法高亮
void select_highlight() 
{
    G.syntax = NULL;
    if (G.filename == NULL)
    {
        return;
    }

    char *ext = strrchr(G.filename, '.');

    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) 
    {
        struct editor_syntax *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i]) 
        {
            int is_ext = (s->filematch[i][0] == '.');
            if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
                (!is_ext && strstr(G.filename, s->filematch[i])))
            {
                G.syntax = s;

                int filerow;
                for (filerow = 0; filerow < G.numrows; filerow++) 
                {
                update_syntax(&G.row[filerow]);
                }
                return;
            }
            i++;
        }
    }
}

/*-----------------------初始化-------------------------*/

void init() 
{
    G.cx = 0;
    G.cy = 0;
    G.rx = 0;
    G.rowoff = 0;
    G.coloff = 0;
    G.numrows = 0;
    G.row = NULL;
    G.dirty = 0;
    G.filename = NULL;
    G.statusmsg[0] = '\0';
    G.statusmsg_time = 0;
    G.syntax = NULL;

    if (window_size(&G.screenrows, &G.screencols) == -1)
    {
        warn("window_size");
    }
    G.screenrows -= 2;
}


int main(int argc, char *argv[]) 
{
    enable_raw_mode();
    init();
    if (argc >= 2) 
    {
        editor_open(argv[1]);
    }

    set_status_message("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-F = find");

    while (1) 
    {
        refresh_screen();
        process_key();
    }

    return 0;
}
