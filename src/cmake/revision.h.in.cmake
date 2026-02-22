#ifndef __REVISION_H__
#define __REVISION_H__
 #define _REVISION                  "@rev_id_str@"
 #define _HASH                      "@rev_hash@"
 #define _DATE                      "@rev_date@"
 #define _BRANCH                    "@rev_branch@"
 #define _CMAKE_COMMAND             R"(@CMAKE_COMMAND@)"
 #define _CMAKE_VERSION             R"(@CMAKE_VERSION@)"
 #define _CMAKE_HOST_SYSTEM         R"(@CMAKE_HOST_SYSTEM_NAME@ @CMAKE_HOST_SYSTEM_VERSION@)"
 #define _SOURCE_DIRECTORY          R"(@CMAKE_SOURCE_DIR@)"
 #define _BUILD_DIRECTORY           R"(@BUILDDIR@)"
 #define _MYSQL_EXECUTABLE          R"(@MYSQL_EXECUTABLE@)"
 #define C9_COMPANYNAME_STR         "C9Core"
 #define C9_LEGALCOPYRIGHT_STR      "(c)2016-@rev_year@ C9Core (based on AzerothCore)"
 #define C9_FILEVERSION             0,0,0
 #define C9_FILEVERSION_STR         "@rev_hash@ @rev_date@ (@rev_branch@ branch)"
 #define C9_PRODUCTVERSION          C9_FILEVERSION
 #define C9_PRODUCTVERSION_STR      C9_FILEVERSION_STR
 /* Backward-compat aliases — remove after all consumers are updated */
 #define AC_COMPANYNAME_STR         C9_COMPANYNAME_STR
 #define AC_LEGALCOPYRIGHT_STR      C9_LEGALCOPYRIGHT_STR
 #define AC_FILEVERSION             C9_FILEVERSION
 #define AC_FILEVERSION_STR         C9_FILEVERSION_STR
 #define AC_PRODUCTVERSION          C9_PRODUCTVERSION
 #define AC_PRODUCTVERSION_STR      C9_PRODUCTVERSION_STR
#endif // __REVISION_H__
