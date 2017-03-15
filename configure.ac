#
#  Copyright (c) 2012 Rob Clark <rob@ti.com>
#
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  the rights to use, copy, modify, merge, publish, distribute, sublicense,
#  and/or sell copies of the Software, and to permit persons to whom the
#  Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice (including the next
#  paragraph) shall be included in all copies or substantial portions of the
#  Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
#  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
#  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
#  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
#  SOFTWARE.
#

# Initialize Autoconf
AC_PREREQ([2.60])
AC_INIT([kmscube], [0.0.1], [https://cgit.freedesktop.org/mesa/kmscube/], [kmscube])
AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_AUX_DIR([build-aux])
AC_CONFIG_MACRO_DIR([m4])

# Initialize Automake
AM_INIT_AUTOMAKE([foreign dist-bzip2])

# Enable quiet compiles on automake 1.11.
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

# Initialize libtool
AC_PROG_LIBTOOL

# Obtain compiler/linker options for depedencies
PKG_CHECK_MODULES(DRM, libdrm)
PKG_CHECK_MODULES(GBM, gbm)
PKG_CHECK_MODULES(EGL, egl)
PKG_CHECK_MODULES(GLES2, glesv2)

# Check for gst and enable cube-video conditionally:
PKG_CHECK_MODULES(GST, gstreamer-1.0 gstreamer-plugins-base-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0 gstreamer-video-1.0 glib-2.0,
		 [HAVE_GST=yes], [HAVE_GST=no])
if test "x$HAVE_GST" = "xyes"; then
	AC_DEFINE(HAVE_GST, 1, [Have GStreamer support])
	AC_MSG_NOTICE([Building cube-video support])
fi
AM_CONDITIONAL(ENABLE_GST, [test "x$HAVE_GST" = "xyes"])

dnl ===========================================================================
dnl check compiler flags
AC_DEFUN([LIBDRM_CC_TRY_FLAG], [
	AC_MSG_CHECKING([whether $CC supports $1])

	libdrm_save_CFLAGS="$CFLAGS"
	CFLAGS="$CFLAGS $1"

	AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]],[[]])],
						[libdrm_cc_flag=yes],
						[libdrm_cc_flag=no])
	CFLAGS="$libdrm_save_CFLAGS"
	
	if test "x$libdrm_cc_flag" = "xyes"; then
		ifelse([$2], , :, [$2])
	else
		ifelse([$3], , :, [$3])
	fi
	AC_MSG_RESULT([$libdrm_cc_flag])
])

MAYBE_WARN="-Wall -Wextra -Wno-packed-bitfield-compat \
-Wsign-compare -Werror-implicit-function-declaration \
-Wpointer-arith -Wwrite-strings -Wstrict-prototypes \
-Wnested-externs \
-Wpacked -Wswitch-enum -Wmissing-format-attribute \
-Wstrict-aliasing=2 -Winit-self -Wunsafe-loop-optimizations \
-Wdeclaration-after-statement -Wold-style-definition \
-Wno-missing-field-initializers -Wno-unused-parameter \
-Wno-attributes -Wno-long-long -Winline"

AC_MSG_CHECKING([which warning flags were supported])
for W in $MAYBE_WARN; do
	LIBDRM_CC_TRY_FLAG([$W], [WARN_CFLAGS="$WARN_CFLAGS $W"])
done

AC_SUBST(WARN_CFLAGS)

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
