#
#  Copyright (c) 2018 Lyude Paul <thatslyude@gmail.com>
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
project(
  'kmscube',
  'c',
  version : '0.0.1',
  license : 'MIT',
  default_options : ['c_std=c99']
)

if get_option('c_std') != 'c99'
  error('c_std must be c99')
endif

cc = meson.get_compiler('c')
add_project_arguments(
  cc.get_supported_arguments([
    '-Wall',
    '-Wextra'
  ]),
  language: 'c'
)

sources = [
  'common.c',
  'cube-smooth.c',
  'cube-tex.c',
  'drm-atomic.c',
  'drm-common.c',
  'drm-legacy.c',
  'esTransform.c',
  'frame-512x512-NV12.c',
  'frame-512x512-RGBA.c',
  'kmscube.c',
]

dep_m = cc.find_library('m', required : false)
dep_threads = dependency('threads', required : true)
dep_libdrm = dependency('libdrm', version : '>=2.4.71', required : true)
dep_gbm = dependency('gbm', version : '>=13.0', required : true)
dep_egl = dependency('egl', required : true)
dep_gles2 = dependency('glesv2', required : true)

dep_common = [dep_m, dep_threads, dep_libdrm, dep_gbm, dep_egl, dep_gles2]

dep_gst = []
with_gst = get_option('gstreamer')
if with_gst != 'false'
  _dep_names = [
    'gstreamer-1.0',
    'gstreamer-plugins-base-1.0',
    'gstreamer-app-1.0',
    'gstreamer-allocators-1.0',
    'gstreamer-video-1.0',
  ]
  _required = with_gst == 'true'
  foreach _dep : _dep_names
    dep_gst += [dependency(_dep, version : '>= 1.6.0', required : _required)]
  endforeach
  dep_gst += [dependency('glib-2.0', required : _required)]

  # See if we found everything we needed
  if with_gst == 'auto'
    foreach _dep : dep_gst
      if not _dep.found()
        with_gst = 'false'
        break
      endif
    endforeach
  endif

  if with_gst != 'false'
    dep_common += dep_gst
    sources += ['cube-video.c', 'gst-decoder.c']
    add_project_arguments('-DHAVE_GST', language : 'c')
  endif
endif

executable('kmscube', sources, dependencies : dep_common, install : true)

if with_gst == 'false'
  message('Building without gstreamer support')
else
  message('Building with gstreamer support')
endif
