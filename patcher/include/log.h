#ifndef __PATCHER_LOG_H__
#define __PATCHER_LOG_H__

#include <stdarg.h>
#include <string.h>
#include <errno.h>

#define LOG_UNSET	(-1)
#define LOG_MSG		(0) /* Print message regardless of log level */
#define LOG_ERROR	(1) /* Errors only, when we're in trouble */
#define LOG_WARN	(2) /* Warnings, dazen and confused but trying to continue */
#define LOG_INFO	(3) /* Informative, everything is fine */
#define LOG_DEBUG	(4) /* Debug only */

#define DEFAULT_LOGLEVEL	LOG_WARN

extern void print_on_level(unsigned int loglevel, const char *format, ...)
	__attribute__ ((__format__ (__printf__, 2, 3)));
extern void __print_on_level(unsigned int loglevel, const char *format, va_list params);

extern int log_get_fd(void);
extern int log_init(const char *output);
extern void log_fini(void);
extern void log_set_loglevel(unsigned int level);
extern unsigned int log_get_loglevel(void);

#ifndef LOG_PREFIX
# define LOG_PREFIX
#endif

#define print_once(loglevel, fmt, ...)					\
	do {								\
		static bool __printed;					\
		if (!__printed) {					\
			print_on_level(loglevel, fmt, ##__VA_ARGS__);	\
			__printed = 1;					\
		}							\
	} while (0)

#define pr_msg(fmt, ...)						\
	print_on_level(LOG_MSG,						\
		       fmt, ##__VA_ARGS__)

#define pr_info(fmt, ...)						\
	print_on_level(LOG_INFO,					\
		       LOG_PREFIX fmt, ##__VA_ARGS__)

#define pr_err(fmt, ...)						\
	print_on_level(LOG_ERROR,					\
		       "Error (%s +%d): " LOG_PREFIX fmt,		\
		       __FILE__, __LINE__, ##__VA_ARGS__)

#define pr_err_once(fmt, ...)						\
	print_once(LOG_ERROR, fmt, ##__VA_ARGS__)

#define pr_warn(fmt, ...)						\
	print_on_level(LOG_WARN,					\
		       "Warn  (%s +%d): " LOG_PREFIX fmt,		\
		       __FILE__, __LINE__, ##__VA_ARGS__)

#define pr_warn_once(fmt, ...)						\
	print_once(LOG_WARN, fmt, ##__VA_ARGS__)

#define pr_debug(fmt, ...)						\
	print_on_level(LOG_DEBUG,					\
		       LOG_PREFIX fmt, ##__VA_ARGS__)

#define pr_perror(fmt, ...)						\
	pr_err(fmt ": %s\n", ##__VA_ARGS__, strerror(errno))

#endif /* __PATCHER_LOG_H__ */
