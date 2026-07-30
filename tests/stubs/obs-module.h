#pragma once

#include <stdarg.h>

#define LOG_DEBUG 100
#define LOG_INFO 200
#define LOG_WARNING 300
#define LOG_ERROR 400

static inline void blog(int level, const char *format, ...)
{
	(void)level;
	(void)format;
}
