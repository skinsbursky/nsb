/*
 * Copyright (c) 2016-2017, Parallels International GmbH
 *
 * Our contact details: Parallels International GmbH, Vordergasse 59, 8200
 * Schaffhausen, Switzerland.
 */

#ifndef __NSB_TESTS_TYPES__
#define __NSB_TESTS_TYPES__

#define TEST_ERROR	0xDEADDEAD
#define TEST_FAILED	0xDEADBEAF

typedef enum {
	TEST_TYPE_GLOBAL_FUNC,
	TEST_TYPE_STATIC_FUNC_MANUAL,
	TEST_TYPE_EXT_GLOBAL_FUNC,
	TEST_TYPE_GLOBAL_FUNC_CB_MANUAL,
	TEST_TYPE_GLOBAL_FUNC_P,

	TEST_TYPE_GLOBAL_VAR_MANUAL,
	TEST_TYPE_GLOBAL_VAR_ADDR_MANUAL,

	TEST_TYPE_STATIC_VAR_MANUAL,

	TEST_TYPE_CONST_VAR,

	TEST_TYPE_STATIC_FUNC_AUTO,
	TEST_TYPE_GLOBAL_FUNC_CB_AUTO,

	TEST_TYPE_GLOBAL_VAR_AUTO,
	TEST_TYPE_GLOBAL_VAR_ADDR_AUTO,

	TEST_TYPE_STATIC_VAR_AUTO,

	TEST_TYPE_MAX,
} test_type_t;

#define RESULT_CODE			0x0000C0FFEE000000UL

#define original_result(type)		(RESULT_CODE + type)
#define patched_result(type)		(original_result(type) + TEST_TYPE_MAX)

static inline unsigned long __attribute__((always_inline)) function_result(test_type_t type)
{
#ifdef PATCH
	return patched_result(type);
#else
	return original_result(type);
#endif
}

#endif
