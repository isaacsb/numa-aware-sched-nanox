AC_INIT([nanox-locality], [0.1], [isaac.sanchez@bsc.es])
AC_LANG([C++])
AC_CONFIG_SRCDIR([src/plugins/emptyfile.h])
AC_DEFINE_UNQUOTED([NANOX_LOCALITY_CONFIGURE_ARGS], ["$0 $@"], [Configure line])

# Environment
AC_CANONICAL_BUILD
AC_CANONICAL_SYSTEM
AC_CANONICAL_TARGET

# Automake initialization
AM_INIT_AUTOMAKE([-Wall -Wno-portability subdir-objects foreign])
AC_CONFIG_MACRO_DIR([m4])

# Enable silent rules if available
supported_silent_rules=1
m4_ifdef([AM_SILENT_RULES],
         [AM_SILENT_RULES([yes])],
         [supported_silent_rules=0])

AM_CONDITIONAL([SUPPORTED_SILENT_RULES], test x$supported_silent_rules = x1)

AC_ARG_WITH([nanox],
	AS_HELP_STRING([--with-nanox=dir], [Directory of Nanos++ installation]),
	[
	 nanox_include="${withval}/include/nanox"
	 nanox_devinclude="${withval}/include/nanox-dev"
         nanox_lib="${withval}/lib"
         nanox="${withval}"
         if test -d "${withval}/lib64";
         then
         nanox_lib="${withval}/lib64"
         fi
        ]
)

AC_ARG_WITH([nanox-include],
	AS_HELP_STRING([--with-nanox-include=dir], [Directory of Nanos++ headers]),
	[
         nanox_include="${withval}"
	]
)

AC_ARG_WITH([nanox-devinclude],
	AS_HELP_STRING([--with-nanox-devinclude=dir], [Directory of Nanos++ dev headers]),
	[
         nanox_include="${withval}"
	]
)

AC_ARG_WITH([nanox-lib],
	AS_HELP_STRING([--with-nanox-lib=dir], [Directory of Nanos++ libraries]),
	[
	 nanox_lib="${withval}"
	]
)


NANOX_INCLUDES="${nanox_include}"
NANOX_DEVINCLUDES="${nanox_devinclude}"
NANOX_LIBS="${nanox_lib}"
NANOX="${nanox}"

AM_CONDITIONAL([NANOX_CONF], [test -f "${nanox_devinclude}/nanox-config.h"])
AM_COND_IF([NANOX_CONF],
	   [],
	   [AC_MSG_ERROR([nanox-config.h file not found! Please update your Nanos++ install and set the parameters accordingly])])


AC_SUBST([NANOX])
AC_SUBST([NANOX_INCLUDES])
AC_SUBST([NANOX_DEVINCLUDES])
AC_SUBST([NANOX_LIBS])

# Check for libscotch support
AC_ARG_WITH([scotch],
	AS_HELP_STRING([--with-scotch=dir], [Directory of SCOTCH graph partitioning library installation (v6.0+)]),
	[
		scotch_h=$withval/include/scotch.h
		if test -e $scotch_h ;
		then
			scotch_version_string=`sed -ne '/^\s*#\s*define\s\+SCOTCH_VERSION .*$/p' $scotch_h`
			scotch_version=`echo $scotch_version_string | sed -e 's/#define SCOTCH_VERSION \([0-9]\+\).*$/\1/'`
		
			if [[[ "$scotch_version" -ge 6 ]]];
			then
				SCOTCH_HOME="$withval"
				scotch=yes
				AC_MSG_RESULT([checking if SCOTCH library is compatible... yes])
			else
				AC_MSG_ERROR([checking if SCOTCH library is compatible... no (libscotch >= 6 needed)])
			fi
		else
			AC_MSG_ERROR([checking if SCOTCH library is compatible... no ($scotch_h file not found)])
		fi
	]
)

AC_SUBST([SCOTCH_HOME])	
AM_CONDITIONAL([SCOTCH], [test "$scotch" = "yes"])

AC_ARG_WITH([metapart],
	AS_HELP_STRING([--with-metapart=dir], [Directory of METAPART graph partitioning library installation (v6.0+)]),
	[
		metapart_h=$withval/include/metapart.h
		if test -e $metapart_h ;
		then
			METAPART_HOME="$withval"
			metapart=yes
			AC_MSG_RESULT([checking if METAPART library is compatible... yes])
		else
			AC_MSG_ERROR([checking if METAPART library is compatible... no])
		fi
	]
)

AC_SUBST([METAPART_HOME])	
AM_CONDITIONAL([METAPART], [test "$metapart" = "yes"])
 
# Nanos version check
# Make sure build_version is double-quoted!
AS_IF([test -d "$srcdir/.git"],[
   # Git directory found
   AC_PATH_PROG([GIT], [git], [])
   AC_MSG_CHECKING([revision of git repository])

   # If git executable is found in path read HEAD version
   AS_IF([test "$GIT" != ""],[
      echo "$GIT" "$srcdir" >& AS_MESSAGE_LOG_FD
      gitversion=$($GIT --git-dir=$srcdir/.git log --pretty='format:%h %ci' -1)
      gitbranch=$($GIT --git-dir=$srcdir/.git rev-parse --abbrev-ref HEAD)
      build_version="\"git $gitbranch $gitversion developer version\""
   ],[
       # Git executable not found
       build_version="\"unknown git commit\""
   ])
   AC_MSG_RESULT([$build_version])
],[
  AS_IF([test -e "$srcdir/VERSION"],[
    AC_MSG_CHECKING([revision of distributed release])
    run_gitversion=`cat $srcdir/VERSION`
    build_version="\"$run_gitversion\""
    AC_MSG_RESULT([$build_version])
  ],[
    build_version="\"unknown revision\""
  ])
])
AC_SUBST([NANOX_LOCALITY_BUILD_VERSION],[$build_version])
AC_DEFINE_UNQUOTED([NANOX_LOCALITY_BUILD_VERSION], [$build_version], [Build version])

AC_DEFINE([MYSETTINGS], [], [Test variable])

# Set compiler default flags
: ${CFLAGS=""}
: ${CXXFLAGS=""}

# ------------------
# Check for programs
# ------------------
 AC_PROG_CC
 AC_PROG_CPP
 
 AC_PROG_CXX
 AC_PROG_CXXCPP
 
 AC_PROG_LD
 AM_PROG_AS
 
 AC_PROG_SED

 # AM_PROG_AR must be called before LT_INIT or a warning ensues
 m4_ifdef([AM_PROG_AR], [AM_PROG_AR])
 # Turn off static libraries.
 LT_INIT([disable-static])

# --------------------------------
# Compiler and flag checks
# --------------------------------

# Check for header files
AC_HEADER_STDC

# Check for typedefs, structures, and compiler characteristics
AC_C_CONST

# Compiler dependent Configuration flags
AX_CONFIG_CC

## -----------------------
## Check host architecture
## -----------------------
OPTIONS=

AC_SUBST([OPTIONS])

# -------------------
# Check for libraries
# -------------------

# Pthreads
AX_PTHREAD

# Check hwloc availability
AX_CHECK_HWLOC

# Build versions
AX_BUILD_VERSIONS

# Generate plugin list
PLUGINS="plugins"
AC_SUBST([PLUGINS])

# Output files
AC_CONFIG_HEADERS([config.h])

AC_CONFIG_FILES([
                 Makefile
                 src/Makefile
                 src/plugins/Makefile
       ])

AC_OUTPUT

gcc_builtins_used="unknown"
AS_IF([test "$new_gcc_builtins" = yes],[
  gcc_builtins_used="new gcc __atomic builtins"
],[
  gcc_builtins_used="legacy gcc __sync builtins"
])

AC_DEFUN([ax_check_enabled],[
  AS_IF([test x$1 == xyes],[
    echo -n "enabled"
  ],[
    echo -n "disabled"
  ])
])
