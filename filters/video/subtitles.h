enum {
	CSRI_F_RGBA = 0,
	CSRI_F_ARGB,
	CSRI_F_BGRA,
	CSRI_F_ABGR,

	CSRI_F_RGB_ = 0x100,
	CSRI_F__RGB,
	CSRI_F_BGR_,			/**< Windows "RGB32" */
	CSRI_F__BGR,

	CSRI_F_RGB  = 0x200,
	CSRI_F_BGR,			/**< Windows "RGB24" */

	CSRI_F_AYUV = 0x1000,
	CSRI_F_YUVA,
	CSRI_F_YVUA,

	CSRI_F_YUY2 = 0x1100,

	CSRI_F_YV12A = 0x2011,		/**< planar YUV 2x2 + alpha plane */
	CSRI_F_YV12 = 0x2111,		/**< planar YUV 2x2 */
	CSRI_F_NV12,
	CSRI_F_NV21
};

typedef struct {
	unsigned pixfmt;
	unsigned width;
	unsigned height;
} csri_fmt;

typedef struct {
	unsigned pixfmt;
	unsigned char *planes[4];
	ptrdiff_t strides[4];
} csri_frame;

typedef struct {
	const char *name;
	const char *specific;
	const char *longname;
	const char *author;
	const char *copyright;
} csri_info;

typedef union {
	int32_t lval;
	double dval;
	const char *utf8val;
	void *otherval;
} csri_vardata;

typedef struct {
	const char *name;
	csri_vardata data;
	struct csri_openflag *next;
} csri_openflag;

typedef void* (*csri_open_file_t)(void *renderer, const char *filename, csri_openflag *flags);
typedef int (*csri_add_file_t)(void *inst, const char *filename, csri_openflag *flags);
typedef void (*csri_close_t)(void *inst);
typedef int (*csri_request_fmt_t)(void *inst, const csri_fmt *fmt);
typedef void (*csri_render_t)(void *inst, csri_frame *frame, double time);

extern csri_render_t csri_render;
#define subtitles_render_frame csri_render
extern csri_close_t csri_close;
#define subtitles_close csri_close

void* subtitles_new_renderer(const csri_fmt *fmt, uint32_t sarw, uint32_t sarh);
