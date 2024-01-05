/* See LICENSE for license details. */

#include <stdint.h>
#include <sys/types.h>

/* macros */
#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))
#define LEN(a)			(sizeof(a) / sizeof(a)[0])
#define BETWEEN(x, a, b)	((a) <= (x) && (x) <= (b))
#define DIVCEIL(n, d)		(((n) + ((d) - 1)) / (d))
#define DEFAULT(a, b)		(a) = (a) ? (a) : (b)
#define LIMIT(x, a, b)		(x) = (x) < (a) ? (a) : (x) > (b) ? (b) : (x)
#define ATTRCMP(a, b)		((a).mode != (b).mode || (a).fg != (b).fg || \
				(a).bg != (b).bg || (a).decor != (b).decor)
#define TIMEDIFF(t1, t2)	((t1.tv_sec-t2.tv_sec)*1000 + \
				(t1.tv_nsec-t2.tv_nsec)/1E6)
#define MODBIT(x, set, bit)	((set) ? ((x) |= (bit)) : ((x) &= ~(bit)))

#define TRUECOLOR(r,g,b)	(1 << 24 | (r) << 16 | (g) << 8 | (b))
#define IS_TRUECOL(x)		(1 << 24 & (x))

#define DECOR_UNSET		0xffffffff
#define IS_DECOR_UNSET(x)	((0xffffffff & (x)) == 0xffffffff)

enum glyph_attribute {
	ATTR_NULL       = 0,
	ATTR_BOLD       = 1 << 0,
	ATTR_FAINT      = 1 << 1,
	ATTR_ITALIC     = 1 << 2,
	ATTR_UNDERLINE  = 1 << 3,
	ATTR_BLINK      = 1 << 4,
	ATTR_REVERSE    = 1 << 5,
	ATTR_INVISIBLE  = 1 << 6,
	ATTR_STRUCK     = 1 << 7,
	ATTR_WRAP       = 1 << 8,
	ATTR_WIDE       = 1 << 9,
	ATTR_WDUMMY     = 1 << 10,
	ATTR_BOLD_FAINT = ATTR_BOLD | ATTR_FAINT,
	ATTR_IMAGE      = 1 << 11,
};

enum selection_mode {
	SEL_IDLE = 0,
	SEL_EMPTY = 1,
	SEL_READY = 2
};

enum selection_type {
	SEL_REGULAR = 1,
	SEL_RECTANGULAR = 2
};

enum selection_snap {
	SNAP_WORD = 1,
	SNAP_LINE = 2
};

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

typedef uint_least32_t Rune;

#define Glyph Glyph_
typedef struct {
	Rune u;           /* character code */
	ushort mode;      /* attribute flags */
	uint32_t fg;      /* foreground  */
	uint32_t bg;      /* background  */
	uint32_t decor;   /* decoration (like underline) */
} Glyph;

typedef Glyph *Line;

typedef union {
	int i;
	uint ui;
	float f;
	const void *v;
	const char *s;
} Arg;

void die(const char *, ...);
void redraw(void);
void draw(void);

void printscreen(const Arg *);
void printsel(const Arg *);
void sendbreak(const Arg *);
void toggleprinter(const Arg *);

int tattrset(int);
void tnew(int, int);
void tresize(int, int);
void tsetdirtattr(int);
void ttyhangup(void);
int ttynew(const char *, char *, const char *, char **);
size_t ttyread(void);
void ttyresize(int, int);
void ttywrite(const char *, size_t, int);

void resettitle(void);

void selclear(void);
void selinit(void);
void selstart(int, int, int);
void selextend(int, int, int, int);
int selected(int, int);
char *getsel(void);

Glyph getglyphat(int, int);

size_t utf8encode(Rune, char *);

void *xmalloc(size_t);
void *xrealloc(void *, size_t);
char *xstrdup(const char *);

/* config.h globals */
extern char *utmp;
extern char *scroll;
extern char *stty_args;
extern char *vtiden;
extern wchar_t *worddelimiters;
extern int allowaltscreen;
extern int allowwindowops;
extern char *termname;
extern unsigned int tabspaces;
extern unsigned int defaultfg;
extern unsigned int defaultbg;
extern unsigned int defaultcs;

// Some accessors to image placeholder properties stored in `u`:
// - row (1-base) - 9 bits
// - column (1-base) - 9 bits
// - most significant byte of the image id plus 1 - 9 bits (0 means unspecified,
//   don't forget to subtract 1).
// - the original number of diacritics (0, 1, 2, or 3) - 2 bits
// - whether this is a classic (1) or Unicode (0) placeholder - 1 bit
static inline uint32_t tgetimgrow(Glyph *g) { return g->u & 0x1ff; }
static inline uint32_t tgetimgcol(Glyph *g) { return (g->u >> 9) & 0x1ff; }
static inline uint32_t tgetimgid4thbyteplus1(Glyph *g) { return (g->u >> 18) & 0x1ff; }
static inline uint32_t tgetimgdiacriticcount(Glyph *g) { return (g->u >> 27) & 0x3; }
static inline uint32_t tgetisclassicplaceholder(Glyph *g) { return (g->u >> 29) & 0x1; }
static inline void tsetimgrow(Glyph *g, uint32_t row) {
	g->u = (g->u & ~0x1ff) | (row & 0x1ff);
}
static inline void tsetimgcol(Glyph *g, uint32_t col) {
	g->u = (g->u & ~(0x1ff << 9)) | ((col & 0x1ff) << 9);
}
static inline void tsetimg4thbyteplus1(Glyph *g, uint32_t byteplus1) {
	g->u = (g->u & ~(0x1ff << 18)) | ((byteplus1 & 0x1ff) << 18);
}
static inline void tsetimgdiacriticcount(Glyph *g, uint32_t count) {
	g->u = (g->u & ~(0x3 << 27)) | ((count & 0x3) << 27);
}
static inline void tsetisclassicplaceholder(Glyph *g, uint32_t isclassic) {
	g->u = (g->u & ~(0x1 << 29)) | ((isclassic & 0x1) << 29);
}

/// Returns the full image id. This is a naive implementation, if the most
/// significant byte is not specified, it's assumed to be 0 instead of inferring
/// it from the cells to the left.
static inline uint32_t tgetimgid(Glyph *g) {
	uint32_t msb = tgetimgid4thbyteplus1(g);
	if (msb != 0)
		--msb;
	return (msb << 24) | (g->fg & 0xFFFFFF);
}

/// Sets the full image id.
static inline void tsetimgid(Glyph *g, uint32_t id) {
	g->fg = (id & 0xFFFFFF) | (1 << 24);
	tsetimg4thbyteplus1(g, ((id >> 24) & 0xFF) + 1);
}

static inline uint32_t tgetimgplacementid(Glyph *g) {
	if (IS_DECOR_UNSET(g->decor))
		return 0;
	return g->decor & 0xFFFFFF;
}

static inline void tsetimgplacementid(Glyph *g, uint32_t id) {
	g->decor = (id & 0xFFFFFF) | (1 << 24);
}
