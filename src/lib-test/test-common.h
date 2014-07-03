#ifndef TEST_COMMON_H
#define TEST_COMMON_H

struct istream *test_istream_create(const char *data);
struct istream *test_istream_create_data(const void *data, size_t size);
void test_istream_set_size(struct istream *input, uoff_t size);
void test_istream_set_allow_eof(struct istream *input, bool allow);
void test_istream_set_max_buffer_size(struct istream *input, size_t size);

void test_begin(const char *name);
#define test_assert(code) STMT_START { \
	if (!(code)) test_assert_failed(#code, __FILE__, __LINE__); \
	} STMT_END
/* Additional parameter may be int or unsigned int, to indicate which of
 * a barrage of tests have failed (such as in a loop).
 */
#define test_assert_idx(code, i) STMT_START { \
		if (!(code)) test_assert_failed_idx(#code, __FILE__, __LINE__, i); \
	} STMT_END
void test_assert_failed(const char *code, const char *file, unsigned int line);
void test_assert_failed_idx(const char *code, const char *file, unsigned int line, long long i);
bool test_has_failed(void);
void test_end(void);

void test_out(const char *name, bool success);
void test_out_quiet(const char *name, bool success); /* only prints failures */
void test_out_reason(const char *name, bool success, const char *reason)
	ATTR_NULL(3);

int test_run(void (*test_functions[])(void));

void test_init(void);
int test_deinit(void);
void test_run_funcs(void (*test_functions[])(void));

#endif
