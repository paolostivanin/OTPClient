#  Copyright (C) 2011 Felix Geyer <debfx@fobos.de>
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 2 or (at your option)
#  version 3 of the License.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.

find_path(LIBZIP_INCLUDE_DIR zip.h)

find_library(LIBZIP_LIBRARIES zip)

mark_as_advanced(LIBZIP_LIBRARIES LIBZIP_INCLUDE_DIR)

if(GCRYPT_INCLUDE_DIR AND EXISTS "${LIBZIP_INCLUDE_DIR}/zip.h")
    file(STRINGS "${LIBZIP_INCLUDE_DIR}/zip.h" LIBZIP_H REGEX "^#define LIBZIP_VERSION \"[^\"]*\"$")
    string(REGEX REPLACE "^.*LIBZIP_VERSION \"([0-9]+).*$" "\\1" LIBZIP_VERSION_MAJOR "${LIBZIP_H}")
    string(REGEX REPLACE "^.*LIBZIP_VERSION \"[0-9]+\\.([0-9]+).*$" "\\1" LIBZIP_VERSION_MINOR  "${LIBZIP_H}")
    string(REGEX REPLACE "^.*LIBZIP_VERSION \"[0-9]+\\.[0-9]+\\.([0-9]+).*$" "\\1" LIBZIP_VERSION_PATCH "${LIBZIP_H}")
    set(LIBZIP_VERSION_STRING "${LIBZIP_VERSION_MAJOR}.${LIBZIP_VERSION_MINOR}.${LIBZIP_VERSION_PATCH}")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Libzip DEFAULT_MSG LIBZIP_LIBRARIES LIBZIP_INCLUDE_DIR)
