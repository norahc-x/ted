/* syntax.c — highlighting engine and the language database. */
#include "editor.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const char *C_extensions[] = { ".c", ".h", ".cpp", ".hpp", ".cc", NULL };
static const char *C_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "default", "goto", "sizeof", "volatile", "register", "extern", "inline",
    "do", "restrict",
    /* trailing | marks type keywords (second color) */
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", "short|", "const|", "bool|", "size_t|", "ssize_t|", "uint8_t|",
    "uint16_t|", "uint32_t|", "uint64_t|", "int8_t|", "int16_t|", "int32_t|",
    "int64_t|", "FILE|", "time_t|", NULL
};

static const char *PY_extensions[] = { ".py", NULL };
static const char *PY_keywords[] = {
    "def", "class", "import", "from", "return", "if", "elif", "else", "for",
    "while", "try", "except", "finally", "with", "as", "lambda", "pass",
    "break", "continue", "raise", "yield", "in", "is", "not", "and", "or",
    "assert", "del", "global", "nonlocal", "async", "await", "match",
    "True|", "False|", "None|", "self|", "int|", "str|", "float|", "list|",
    "dict|", "set|", "tuple|", "bool|", "bytes|", "len|", "range|", "print|",
    NULL
};

static const char *SH_extensions[] = { ".sh", ".bash", ".bashrc", NULL };
static const char *SH_keywords[] = {
    "if", "then", "else", "elif", "fi", "for", "in", "do", "done", "while",
    "until", "case", "esac", "function", "select",
    "local|", "return|", "export|", "shift|", "exit|", "source|", "alias|",
    "set|", "unset|", "read|", "echo|", "cd|", "test|", "trap|", "eval|",
    "exec|", "printf|", NULL
};

static const char *MK_filematch[] = { "Makefile", "makefile", ".mk", NULL };
static const char *MK_keywords[] = {
    "include", "ifeq", "ifneq", "ifdef", "ifndef", "endif", "else", "define",
    "endef", "export", "unexport", "override", "vpath",
    ".PHONY|", ".SUFFIXES|", ".DEFAULT|", ".PRECIOUS|", ".SECONDARY|",
    ".SILENT|", NULL
};

static const char *JSON_extensions[] = { ".json", NULL };
static const char *JSON_keywords[] = { "true|", "false|", "null|", NULL };

/* One keyword list, two dialect entries: NASM (';') and GAS ('#'). */
static const char *ASM_nasm_extensions[] = { ".asm", ".nasm", NULL };
static const char *ASM_gas_extensions[] = { ".s", ".S", NULL };
static const char *ASM_keywords[] = {
    "mov", "movb", "movw", "movl", "movq", "movzx", "movsx", "lea", "leal",
    "leaq", "push", "pushl", "pushq", "pop", "popl", "popq", "call", "ret",
    "jmp", "je", "jne", "jz", "jnz", "jg", "jge", "jl", "jle", "ja", "jae",
    "jb", "jbe", "js", "jns", "cmp", "cmpl", "cmpq", "test", "add", "addl",
    "addq", "sub", "subl", "subq", "mul", "imul", "div", "idiv", "inc",
    "dec", "and", "or", "xor", "not", "neg", "shl", "shr", "sar", "nop",
    "int", "syscall", "leave", "enter", "cdq", "cqo", "loop",
    "section", "segment", "global", "extern", "db", "dw", "dd", "dq",
    "resb", "resw", "resd", "equ", "org", "bits", "align",
    ".text", ".data", ".bss", ".globl", ".global", ".align", ".byte",
    ".word", ".long", ".quad", ".ascii", ".asciz", ".string", ".section",
    ".type", ".size",
    "rax|", "rbx|", "rcx|", "rdx|", "rsi|", "rdi|", "rbp|", "rsp|", "rip|",
    "eax|", "ebx|", "ecx|", "edx|", "esi|", "edi|", "ebp|", "esp|",
    "ax|", "bx|", "cx|", "dx|", "si|", "di|", "bp|", "sp|",
    "al|", "bl|", "cl|", "dl|", "ah|", "bh|", "ch|", "dh|",
    "r8|", "r9|", "r10|", "r11|", "r12|", "r13|", "r14|", "r15|", NULL
};

static struct esyntax HLDB[] = {
    { "c", C_extensions, C_keywords, "//", "/*", "*/", HL_NUMBERS | HL_STRINGS },
    { "python", PY_extensions, PY_keywords, "#", NULL, NULL, HL_NUMBERS | HL_STRINGS },
    { "shell", SH_extensions, SH_keywords, "#", NULL, NULL, HL_NUMBERS | HL_STRINGS },
    { "makefile", MK_filematch, MK_keywords, "#", NULL, NULL, HL_NUMBERS | HL_STRINGS },
    { "json", JSON_extensions, JSON_keywords, NULL, NULL, NULL, HL_NUMBERS | HL_STRINGS },
    { "asm", ASM_nasm_extensions, ASM_keywords, ";", NULL, NULL, HL_NUMBERS | HL_STRINGS },
    { "asm", ASM_gas_extensions, ASM_keywords, "#", "/*", "*/", HL_NUMBERS | HL_STRINGS },
};
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

static int isSeparator(int c) {
    return c == '\0' || isspace(c) || strchr(",.()+-/*=~%<>[];{}:&|!?", c) != NULL;
}

/* Recompute row->hl; returns 1 if the open-comment state changed. */
static int syntaxRow(struct ebuf *b, struct erow *row) {
    struct esyntax *s = b->syntax;
    const char **keywords;
    int scs_len, mcs_len, mce_len;
    int prev_sep = 1, in_string = 0, in_comment, changed, i = 0;

    row->hl = xrealloc(row->hl, (size_t)(row->rsize ? row->rsize : 1));
    memset(row->hl, HL_NORMAL, (size_t)row->rsize);
    if (!s) {
        changed = (row->hl_open_comment != 0);
        row->hl_open_comment = 0;
        return changed;
    }
    keywords = s->keywords;
    scs_len = s->scs ? (int)strlen(s->scs) : 0;
    mcs_len = s->mcs ? (int)strlen(s->mcs) : 0;
    mce_len = s->mce ? (int)strlen(s->mce) : 0;
    in_comment = (row->idx > 0 && b->rows[row->idx - 1].hl_open_comment);

    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment &&
            !strncmp(&row->render[i], s->scs, (size_t)scs_len)) {
            memset(&row->hl[i], HL_COMMENT, (size_t)(row->rsize - i));
            break;
        }

        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->hl[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], s->mce, (size_t)mce_len)) {
                    memset(&row->hl[i], HL_MLCOMMENT, (size_t)mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_sep = 1;
                } else {
                    i++;
                }
                continue;
            } else if (!strncmp(&row->render[i], s->mcs, (size_t)mcs_len)) {
                memset(&row->hl[i], HL_MLCOMMENT, (size_t)mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (s->flags & HL_STRINGS) {
            if (in_string) {
                row->hl[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) {
                    row->hl[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_sep = 1;
                continue;
            } else if (c == '"' || c == '\'') {
                in_string = c;
                row->hl[i] = HL_STRING;
                i++;
                continue;
            }
        }

        if (s->flags & HL_NUMBERS) {
            if ((isdigit((unsigned char)c) &&
                 (prev_sep || prev_hl == HL_NUMBER)) ||
                (c == '.' && prev_hl == HL_NUMBER)) {
                row->hl[i] = HL_NUMBER;
                i++;
                prev_sep = 0;
                continue;
            }
        }

        if (prev_sep) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = (int)strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;
                if (!strncmp(&row->render[i], keywords[j], (size_t)klen) &&
                    isSeparator((unsigned char)row->render[i + klen])) {
                    memset(&row->hl[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1,
                           (size_t)klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_sep = 0;
                continue;
            }
        }

        prev_sep = isSeparator((unsigned char)c);
        i++;
    }

    changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    return changed;
}

/* Update a row and propagate multiline-comment state forward (iteratively,
 * unlike kilo's unbounded recursion). */
void editorUpdateSyntax(struct ebuf *b, struct erow *row) {
    while (row) {
        int changed = syntaxRow(b, row);
        row = (changed && row->idx + 1 < b->numrows)
                  ? &b->rows[row->idx + 1] : NULL;
    }
}

int editorSyntaxToColor(int hl) {
    switch (hl) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return 36;
    case HL_KEYWORD1:  return 33;
    case HL_KEYWORD2:  return 32;
    case HL_STRING:    return 35;
    case HL_NUMBER:    return 31;
    case HL_MATCH:     return 34;
    default:           return 39;
    }
}

void editorSelectSyntaxHighlight(struct ebuf *b) {
    size_t j;
    int i, r;
    char *ext;

    b->syntax = NULL;
    if (b->filename) {
        ext = strrchr(b->filename, '.');
        for (j = 0; j < HLDB_ENTRIES && !b->syntax; j++) {
            for (i = 0; HLDB[j].filematch[i]; i++) {
                const char *pat = HLDB[j].filematch[i];
                if (pat[0] == '.' ? (ext && !strcmp(ext, pat))
                                  : (strstr(b->filename, pat) != NULL)) {
                    b->syntax = &HLDB[j];
                    break;
                }
            }
        }
    }
    for (r = 0; r < b->numrows; r++) editorUpdateSyntax(b, &b->rows[r]);
}
