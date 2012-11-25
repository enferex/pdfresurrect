#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.61)
AC_INIT


# Install prefix
AC_PREFIX_DEFAULT(/usr/local)


# Checks for programs.
AC_PROG_CC


# Checks for header files.
AC_HEADER_DIRENT
AC_HEADER_STDC
AC_CHECK_HEADERS([stdlib.h string.h])


# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_SIZE_T


# Check for functions
AC_CHECK_FUNCS([strdup strtol])


# Macros
CFLAGS="$CFLAGS -Wall"
AC_ARG_ENABLE([debug],
              [AC_HELP_STRING([--enable-debug], [Enable debugging output])],
              [CFLAGS="$CFLAGS -O0 -g3 -DDEBUG"],
              [CFLAGS="$CFLAGS -O3"])

AC_SUBST(CFLAGS)


AC_FUNC_CLOSEDIR_VOID
AC_FUNC_MALLOC
AC_FUNC_MEMCMP
AC_FUNC_REALLOC
AC_CHECK_FUNCS([memset mkdir strchr strrchr strstr])


AC_CONFIG_FILES([Makefile])
AC_OUTPUT
